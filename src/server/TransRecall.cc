#include "ServerIncludes.h"

/** @page transparent_recall Transparent Recall

    # TransRecall

    The transparent recall processing happens within two phases:

    1. One backend thread ("RecallD" executing TransRecall::run) waits on a
       socket for recall events. Recall events are are initiated by
       applications that perform read, write, or truncate calls on a
       premigrated or migrated files. A corresponding
       job is created within the JOB_QUEUE table and if it does not exist a
       request is created within the REQUEST_QUEUE table.
    2. The Scheduler identifies a transparent recall request to get scheduled.
       The order of files being recalled depends on the starting block of
       the data files on tape: @snippet server/SQLStatements.cc trans_recall_sql_qry
       If the transparent recall job is finally processed (even it is failed)
       the event is responded  as protocol buffer messages
       (LTFSDmProtocol::LTFSDmTransRecResp).

    @dot
    digraph trans_recall {
        compound=true;
        fontname="fixed";
        fontsize=11;
        labeljust=l;
        node [shape=record, width=2, fontname="fixed", fontsize=11, fillcolor=white, style=filled];
        subgraph cluster_first {
            label="first phase";
            recv [label="receive event"];
            ajr [label="add job, add\nrequest if not exists"];
            recv -> ajr [];
        }
        subgraph cluster_second {
            label="second phase";
            scheduler [label="Scheduler"];
            subgraph cluster_rec_exec {
                label="SelRecall::execRequest";
                subgraph cluster_proc_files {
                    label="SelRecall::processFiles";
                    rec_exec [label="{read from tape\n\nordered by starting block|respond event}"];
                }
            }
            scheduler -> rec_exec [label="schedule\nrecall request", fontname="fixed", fontsize=8, lhead=cluster_rec_exec];
        }
        subgraph cluster_tables {
            label="SQLite tables";
            tables [label="<rq> REQUEST_QUEUE|<jq> JOB_QUEUE"];
        }
        ajr ->  tables [color=darkgreen, fontcolor=darkgreen, label="add", fontname="fixed", fontsize=8, headport=w, lhead=cluster_tables];
        scheduler -> tables:rq [color=darkgreen, fontcolor=darkgreen, label="check for requests to schedule", fontname="fixed", fontsize=8, tailport=e];
        rec_exec -> tables [color=darkgreen, fontcolor=darkgreen, label="read and update", fontname="fixed", fontsize=8, lhead=cluster_tables, ltail=cluster_rec_exec];
        ajr -> scheduler [color=blue, fontcolor=blue, label="condition", fontname="fixed", fontsize=8];
        scheduler -> ajr [style=invis]; // just for the correct order of the subgraphs
    }
    @enddot

    This high level description is explained in more detail in the following subsections.

    If there are multiple recall events for files on the same tape
    only one request is created within the REQUEST_QUEUE table. This
    request is removed if there are no further outstanding transparent
    recalls for the same tape. If there is a new transparent recall
    event and if a corresponding request already exists within the
    REQUEST_QUEUE table this existing request is used for further
    processing the event.

    The second step will not start before the first step is completed. For
    the second step the required tape and drive resources need to be
    available: e.g. a corresponding tape cartridge is mounted on a tape drive.
    The second phase may start immediately after the first phase but it also
    can take a longer time depending when a required resource gets available.

    ## 1. adding jobs and requests to the internal tables

    One backend thread exists (see @ref server_code) that executes the
    TransRecall::run method to wait for recall events. Recall events are sent
    as protocol buffer messages (LTFSDmProtocol::LTFSDmTransRecRequest) over a
    socket. The information provided contains the following:

    - opaque information specific to the connector
    - an indicator if a file should be recall to premigrated or to
      resident state
    - the file uid (see fuid_t)
    - the file name

    Thereafter the tape id for the first tape listed within the attributes
    is obtained. The recall will happen from that tape. There currently is
    no optimization if the file has been migrated to more than one tape to
    select between these tapes in an optimal way.

    To add a corresponding job within the JOB_QUEUE table or if necessary
    a request within the REQUEST_QUEUE table an additional thread is used
    as part of the ThreadPool wqr executing the method TransRecall::addJob.



 */

void TransRecall::addJob(Connector::rec_info_t recinfo, std::string tapeId,
        long reqNum)

{
    struct stat statbuf;
    SQLStatement stmt;
    std::string tapeName;
    int state;
    FsObj::mig_attr_t attr;
    std::string filename;
    bool reqExists = false;

    if (recinfo.filename.compare("") == 0)
        filename = "NULL";
    else
        filename = std::string("'") + recinfo.filename + "'";

    try {
        FsObj fso(recinfo);
        statbuf = fso.stat();

        if (!S_ISREG(statbuf.st_mode)) {
            MSG(LTFSDMS0032E, recinfo.fuid.inum);
            return;
        }

        state = fso.getMigState();

        if (state == FsObj::RESIDENT) {
            MSG(LTFSDMS0031I, recinfo.fuid.inum);
            Connector::respondRecallEvent(recinfo, true);
            return;
        }

        attr = fso.getAttribute();

        tapeName = Server::getTapeName(recinfo.fuid.fsid_h, recinfo.fuid.fsid_l,
                recinfo.fuid.igen, recinfo.fuid.inum, tapeId);
    } catch (const std::exception& e) {
        TRACE(Trace::error, e.what());
        if (filename.compare("NULL") != 0)
            MSG(LTFSDMS0073E, filename);
        else
            MSG(LTFSDMS0032E, recinfo.fuid.inum);
    }

    stmt(TransRecall::ADD_JOB) << DataBase::TRARECALL << filename.c_str()
            << reqNum
            << (recinfo.toresident ? FsObj::RESIDENT : FsObj::PREMIGRATED)
            << Const::UNSET << statbuf.st_size << recinfo.fuid.fsid_h
            << recinfo.fuid.fsid_l << recinfo.fuid.igen << recinfo.fuid.inum
            << statbuf.st_mtime << 0 << time(NULL) << state << tapeId
            << Server::getStartBlock(tapeName)
            << (std::intptr_t) recinfo.conn_info;

    TRACE(Trace::normal, stmt.str());

    stmt.doall();

    if (filename.compare("NULL") != 0)
        TRACE(Trace::always, filename);
    else
        TRACE(Trace::always, recinfo.fuid.inum);

    TRACE(Trace::always, tapeId);

    std::unique_lock<std::mutex> lock(Scheduler::mtx);

    stmt(TransRecall::CHECK_REQUEST_EXISTS) << reqNum;
    stmt.prepare();
    while (stmt.step())
        reqExists = true;
    stmt.finalize();

    if (reqExists == true) {
        stmt(TransRecall::CHANGE_REQUEST_TO_NEW) << DataBase::REQ_NEW << reqNum
                << tapeId;
        TRACE(Trace::normal, stmt.str());
        stmt.doall();
        Scheduler::cond.notify_one();
    } else {
        stmt(TransRecall::ADD_REQUEST) << DataBase::TRARECALL << reqNum
                << attr.tapeId[0] << time(NULL) << DataBase::REQ_NEW;
        TRACE(Trace::normal, stmt.str());
        stmt.doall();
        Scheduler::cond.notify_one();
    }
}

void TransRecall::cleanupEvents()

{
    Connector::rec_info_t recinfo;
    SQLStatement stmt = SQLStatement(TransRecall::REMAINING_JOBS)
            << DataBase::TRARECALL;
    TRACE(Trace::normal, stmt.str());
    stmt.prepare();
    while (stmt.step(&recinfo.fuid.fsid_h, &recinfo.fuid.fsid_l,
            &recinfo.fuid.igen, &recinfo.fuid.inum, &recinfo.filename,
            (std::intptr_t *) &recinfo.conn_info)) {
        TRACE(Trace::always, recinfo.filename, recinfo.fuid.inum);
        Connector::respondRecallEvent(recinfo, false);
    }
    stmt.finalize();
}

void TransRecall::run(std::shared_ptr<Connector> connector)

{
    ThreadPool<TransRecall, Connector::rec_info_t, std::string, long> wqr(
            &TransRecall::addJob, Const::MAX_TRANSPARENT_RECALL_THREADS,
            "trec-wq");
    Connector::rec_info_t recinfo;
    std::map<std::string, long> reqmap;
    std::string tapeId;

    try {
        connector->initTransRecalls();
    } catch (const std::exception& e) {
        TRACE(Trace::error, e.what());
        MSG(LTFSDMS0030E);
        return;
    }

    try {
        FileSystems fss;
        for (std::string fs : Server::conf.getFss()) {
            try {
                FsObj fileSystem(fs);
                if (fileSystem.isFsManaged()) {
                    MSG(LTFSDMS0042I, fs);
                    fileSystem.manageFs(true, connector->getStartTime());
                }
            } catch (const LTFSDMException& e) {
                TRACE(Trace::error, e.what());
                switch (e.getError()) {
                    case Error::FS_CHECK_ERROR:
                        MSG(LTFSDMS0044E, fs);
                        break;
                    case Error::FS_ADD_ERROR:
                        MSG(LTFSDMS0045E, fs);
                        break;
                    default:
                        MSG(LTFSDMS0045E, fs);
                }
            } catch (const std::exception& e) {
                TRACE(Trace::error, e.what());
            }
        }
    } catch (const std::exception& e) {
        MSG(LTFSDMS0079E, e.what());
    }

    while (Connector::connectorTerminate == false) {
        try {
            recinfo = connector->getEvents();
        } catch (const std::exception& e) {
            MSG(LTFSDMS0036W, e.what());
        }

        // is sent for termination
        if (recinfo.conn_info == NULL) {
            TRACE(Trace::always, recinfo.fuid.inum);
            continue;
        }

        if (Server::terminate == true) {
            TRACE(Trace::always, (bool) Server::terminate);
            connector->respondRecallEvent(recinfo, false);
            continue;
        }

        if (recinfo.fuid.inum == 0) {
            TRACE(Trace::always, recinfo.fuid.inum);
            continue;
        }

        // error case: managed region set but no attrs
        try {
            FsObj fso(recinfo);

            if (fso.getMigState() == FsObj::RESIDENT) {
                fso.finishRecall(FsObj::RESIDENT);
                MSG(LTFSDMS0039I, recinfo.fuid.inum);
                connector->respondRecallEvent(recinfo, true);
                continue;
            }

            tapeId = fso.getAttribute().tapeId[0];
        } catch (const LTFSDMException& e) {
            TRACE(Trace::error, e.what());
            if (e.getError() == Error::ATTR_FORMAT)
                MSG(LTFSDMS0037W, recinfo.fuid.inum);
            else
                MSG(LTFSDMS0038W, recinfo.fuid.inum, e.getErrno());
            connector->respondRecallEvent(recinfo, false);
            continue;
        } catch (const std::exception& e) {
            TRACE(Trace::error, e.what());
            connector->respondRecallEvent(recinfo, false);
            continue;
        }

        std::stringstream thrdinfo;
        thrdinfo << "TrRec(" << recinfo.fuid.inum << ")";

        if (reqmap.count(tapeId) == 0)
            reqmap[tapeId] = ++globalReqNumber;

        TRACE(Trace::always, recinfo.fuid.inum, tapeId, reqmap[tapeId]);

        wqr.enqueue(Const::UNSET, TransRecall(), recinfo, tapeId,
                reqmap[tapeId]);
    }

    MSG(LTFSDMS0083I);
    connector->endTransRecalls();
    wqr.waitCompletion(Const::UNSET);
    cleanupEvents();
    MSG(LTFSDMS0084I);
}

unsigned long TransRecall::recall(Connector::rec_info_t recinfo,
        std::string tapeId, FsObj::file_state state, FsObj::file_state toState)

{
    struct stat statbuf;
    struct stat statbuf_tape;
    std::string tapeName;
    char buffer[Const::READ_BUFFER_SIZE];
    long rsize;
    long wsize;
    int fd = -1;
    long offset = 0;
    FsObj::file_state curstate;

    try {
        FsObj target(recinfo);

        TRACE(Trace::always, recinfo.fuid.inum, recinfo.filename);

        std::lock_guard<FsObj> fsolock(target);

        curstate = target.getMigState();

        if (curstate != state) {
            MSG(LTFSDMS0034I, recinfo.fuid.inum);
            state = curstate;
        }
        if (state == FsObj::RESIDENT) {
            return 0;
        } else if (state == FsObj::MIGRATED) {
            tapeName = Server::getTapeName(recinfo.fuid.fsid_h,
                    recinfo.fuid.fsid_l, recinfo.fuid.igen, recinfo.fuid.inum,
                    tapeId);
            fd = open(tapeName.c_str(), O_RDWR | O_CLOEXEC);

            if (fd == -1) {
                TRACE(Trace::error, errno);
                MSG(LTFSDMS0021E, tapeName.c_str());
                THROW(Error::GENERAL_ERROR, tapeName, errno);
            }

            statbuf = target.stat();

            if (fstat(fd, &statbuf_tape) == 0
                    && statbuf_tape.st_size != statbuf.st_size) {
                if (recinfo.filename.size() != 0)
                    MSG(LTFSDMS0097W, recinfo.filename, statbuf.st_size,
                            statbuf_tape.st_size);
                else
                    MSG(LTFSDMS0098W, recinfo.fuid.inum, statbuf.st_size,
                            statbuf_tape.st_size);
                statbuf.st_size = statbuf_tape.st_size;
                toState = FsObj::RESIDENT;
            }

            target.prepareRecall();

            while (offset < statbuf.st_size) {
                if (Server::forcedTerminate)
                    THROW(Error::GENERAL_ERROR, tapeName);

                rsize = read(fd, buffer, sizeof(buffer));
                if (rsize == 0) {
                    break;
                }
                if (rsize == -1) {
                    TRACE(Trace::error, errno);
                    MSG(LTFSDMS0023E, tapeName.c_str());
                    THROW(Error::GENERAL_ERROR, tapeName, errno);
                }
                wsize = target.write(offset, (unsigned long) rsize, buffer);
                if (wsize != rsize) {
                    TRACE(Trace::error, errno, wsize, rsize);
                    MSG(LTFSDMS0033E, recinfo.fuid.inum);
                    close(fd);
                    THROW(Error::GENERAL_ERROR, recinfo.fuid.inum, wsize,
                            rsize);
                }
                offset += rsize;
            }

            close(fd);
        }

        target.finishRecall(toState);
        if (toState == FsObj::RESIDENT)
            target.remAttribute();
    } catch (const std::exception& e) {
        TRACE(Trace::error, e.what());
        if (fd != -1)
            close(fd);
        THROW(Error::GENERAL_ERROR);
    }

    return statbuf.st_size;
}

void TransRecall::processFiles(int reqNum, std::string tapeId)

{
    Connector::rec_info_t recinfo;
    SQLStatement stmt;
    FsObj::file_state state;
    FsObj::file_state toState;
    struct respinfo_t
    {
        Connector::rec_info_t recinfo;bool succeeded;
    };
    std::list<respinfo_t> resplist;
    int numFiles = 0;
    bool succeeded;

    stmt(TransRecall::SET_RECALLING) << FsObj::RECALLING_MIG << reqNum
            << FsObj::MIGRATED << tapeId;
    TRACE(Trace::normal, stmt.str());
    stmt.doall();

    stmt(TransRecall::SET_RECALLING) << FsObj::RECALLING_PREMIG << reqNum
            << FsObj::PREMIGRATED << tapeId;
    TRACE(Trace::normal, stmt.str());
    stmt.doall();

    stmt(TransRecall::SELECT_JOBS) << reqNum << FsObj::RECALLING_MIG
            << FsObj::RECALLING_PREMIG << tapeId;
    TRACE(Trace::normal, stmt.str());
    stmt.prepare();
    while (stmt.step(&recinfo.fuid.fsid_h, &recinfo.fuid.fsid_l,
            &recinfo.fuid.igen, &recinfo.fuid.inum, &recinfo.filename, &state,
            &toState, (std::intptr_t *) &recinfo.conn_info)) {
        numFiles++;

        if (state == FsObj::RECALLING_MIG)
            state = FsObj::MIGRATED;
        else
            state = FsObj::PREMIGRATED;

        if (toState == FsObj::RESIDENT)
            recinfo.toresident = true;

        TRACE(Trace::always, recinfo.filename, recinfo.fuid.inum, state,
                toState);

        try {
            recall(recinfo, tapeId, state, toState);
            succeeded = true;
        } catch (const std::exception& e) {
            TRACE(Trace::error, e.what());
            succeeded = false;
        }

        TRACE(Trace::always, succeeded);
        resplist.push_back((respinfo_t ) { recinfo, succeeded });
    }
    stmt.finalize();
    TRACE(Trace::always, numFiles);

    stmt(TransRecall::DELETE_JOBS) << reqNum << FsObj::RECALLING_MIG
            << FsObj::RECALLING_PREMIG << tapeId;
    TRACE(Trace::normal, stmt.str());
    stmt.doall();

    for (respinfo_t respinfo : resplist)
        Connector::respondRecallEvent(respinfo.recinfo, respinfo.succeeded);
}

void TransRecall::execRequest(int reqNum, std::string tapeId)

{
    SQLStatement stmt;
    int remaining = 0;

    TRACE(Trace::always, reqNum, tapeId);

    processFiles(reqNum, tapeId);

    std::unique_lock<std::mutex> lock(Scheduler::mtx);

    {
        std::lock_guard<std::recursive_mutex> inventorylock(
                LTFSDMInventory::mtx);
        inventory->getCartridge(tapeId)->setState(
                LTFSDMCartridge::TAPE_MOUNTED);
        bool found = false;
        for (std::shared_ptr<LTFSDMDrive> d : inventory->getDrives()) {
            if (d->get_slot() == inventory->getCartridge(tapeId)->get_slot()) {
                TRACE(Trace::normal, d->GetObjectID());
                d->setFree();
                found = true;
                break;
            }
        }
        assert(found == true);
    }

    stmt(TransRecall::COUNT_REMAINING_JOBS) << reqNum << tapeId;
    TRACE(Trace::normal, stmt.str());
    stmt.prepare();
    while (stmt.step(&remaining)) {
    }
    stmt.finalize();

    if (remaining)
        stmt(TransRecall::CHANGE_REQUEST_TO_NEW) << DataBase::REQ_NEW << reqNum
                << tapeId;
    else
        stmt(TransRecall::DELETE_REQUEST) << reqNum << tapeId;
    TRACE(Trace::normal, stmt.str());
    stmt.doall();
    Scheduler::cond.notify_one();
}
