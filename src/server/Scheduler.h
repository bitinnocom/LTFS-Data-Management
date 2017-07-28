#pragma once

class Scheduler

{
private:
	DataBase::operation op;
	int reqNum;
	int tgtState;
	int numRepl;
	int replNum;
	std::string tapeId;
	std::string pool;
	SubServer subs;

	bool poolResAvail(unsigned long minFileSize);
	bool tapeResAvail();
	bool resAvail(unsigned long minFileSize);
	unsigned long smallestMigJob(int reqNum, int replNum);

	static const std::string SELECT_REQUEST;
	static const std::string UPDATE_MIG_REQUEST;
	static const std::string UPDATE_REC_REQUEST;
	static const std::string SMALLEST_MIG_JOB;
public:
	static std::mutex mtx;
	static std::condition_variable cond;
	static std::mutex updmtx;
	static std::condition_variable updcond;
	static std::map<int, std::atomic<bool>> updReq;
	static std::map<std::string, std::atomic<bool>> suspend_map;

	static ThreadPool<Migration::mig_info_t,
			std::shared_ptr<std::list<unsigned long>>> *wqs;

	static std::string getTapeName(std::string fileName, std::string tapeId);
	static std::string getTapeName(unsigned long long fsid, unsigned int igen,
			unsigned long long ino, std::string tapeId);
	static long getStartBlock(std::string tapeName);
	static void mount(std::string driveid, std::string cartridgeid)
	{
		inventory->mount(driveid, cartridgeid);
	}
	static void unmount(std::string driveid, std::string cartridgeid)
	{
		inventory->unmount(driveid, cartridgeid);
	}
	Scheduler()
	{
	}
	~Scheduler()
	{
	}
	;
	void run(long key);
};
