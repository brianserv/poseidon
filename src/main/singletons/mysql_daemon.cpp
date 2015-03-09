// 这个文件是 Poseidon 服务器应用程序框架的一部分。
// Copyleft 2014 - 2015, LH_Mouse. All wrongs reserved.

#include "../precompiled.hpp"
#include "mysql_daemon.hpp"
#include "main_config.hpp"
#include <boost/thread/thread.hpp>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <mysql/mysqld_error.h>
#include "../mysql/object_base.hpp"
#include "../mysql/exception.hpp"
#include "../mysql/connection.hpp"
#include "../atomic.hpp"
#include "../exception.hpp"
#include "../log.hpp"
#include "../raii.hpp"
#include "../multi_index_map.hpp"
#include "../job_base.hpp"
#include "../profiler.hpp"
#include "../utilities.hpp"

namespace Poseidon {

namespace {
	std::string	g_serverAddr		= "localhost";
	unsigned	g_serverPort		= 3306;
	std::string	g_username			= "root";
	std::string	g_password			= "root";
	std::string	g_schema			= "poseidon";
	bool		g_useSsl			= false;
	std::string	g_charset			= "utf8";

	std::string	g_dumpDir			= "/usr/var/poseidon/sqldump";
	std::size_t	g_maxThreads		= 3;
	std::size_t	g_saveDelay			= 5000;
	std::size_t	g_maxReconnDelay	= 60000;
	std::size_t	g_maxRetryCount		= 3;

	// 转储文件，写在最前面。
	boost::mutex g_dumpMutex;
	UniqueFile g_dumpFile;

	// 回调函数任务。
	class SaveCallbackJob : public JobBase {
	private:
		const MySql::AsyncSaveCallback m_callback;
		const bool m_succeeded;
		const boost::uint64_t m_autoId;
		const MySql::ExceptionCallback m_except;

	public:
		SaveCallbackJob(MySql::AsyncSaveCallback callback, bool succeeded, boost::uint64_t autoId,
			MySql::ExceptionCallback except)
			: m_callback(STD_MOVE_IDN(callback)), m_succeeded(succeeded), m_autoId(autoId)
			, m_except(STD_MOVE_IDN(except))
		{
		}

	private:
		boost::weak_ptr<const void> getCategory() const OVERRIDE {
			return VAL_INIT;
		}
		void perform() const OVERRIDE {
			PROFILE_ME;

			try {
				m_callback(m_succeeded, m_autoId);
			} catch(...){
				if(m_except){
					m_except();
				}
				throw;
			}
		}
	};

	class LoadCallbackJob : public JobBase {
	private:
		const MySql::AsyncLoadCallback m_callback;
		const bool m_found;
		const MySql::ExceptionCallback m_except;

	public:
		LoadCallbackJob(MySql::AsyncLoadCallback callback, bool found,
			MySql::ExceptionCallback except)
			: m_callback(STD_MOVE_IDN(callback)), m_found(found)
			, m_except(STD_MOVE_IDN(except))
		{
		}

	private:
		boost::weak_ptr<const void> getCategory() const OVERRIDE {
			return VAL_INIT;
		}
		void perform() const OVERRIDE {
			PROFILE_ME;

			try {
				m_callback(m_found);
			} catch(...){
				if(m_except){
					m_except();
				}
				throw;
			}
		}
	};

	class BatchLoadCallbackJob : public JobBase {
	private:
		const MySql::BatchAsyncLoadCallback m_callback;
		const std::vector<boost::shared_ptr<MySql::ObjectBase> > m_objects;
		const MySql::ExceptionCallback m_except;

	public:
		BatchLoadCallbackJob(MySql::BatchAsyncLoadCallback callback,
			std::vector<boost::shared_ptr<MySql::ObjectBase> > objects,
			MySql::ExceptionCallback except)
			: m_callback(STD_MOVE_IDN(callback)), m_objects(STD_MOVE(objects))
			, m_except(STD_MOVE_IDN(except))
		{
		}

	private:
		boost::weak_ptr<const void> getCategory() const OVERRIDE {
			return VAL_INIT;
		}
		void perform() const OVERRIDE {
			PROFILE_ME;

			try {
				m_callback(m_objects);
			} catch(...){
				if(m_except){
					m_except();
				}
				throw;
			}
		}
	};

	// 数据库线程操作。
	class OperationBase : NONCOPYABLE {
	public:
		virtual ~OperationBase(){
		}

	public:
		// 写入合并。
		virtual boost::shared_ptr<const MySql::ObjectBase> getCombinableObject() const = 0;
		virtual const char *getTableName() const = 0;
		virtual void execute(std::string &query, MySql::Connection &conn) = 0;
	};

	class SaveOperation : public OperationBase {
	private:
		const boost::shared_ptr<const MySql::ObjectBase> m_object;
		const bool m_toReplace;

		MySql::AsyncSaveCallback m_callback;
		const MySql::ExceptionCallback m_except;

	public:
		SaveOperation(boost::shared_ptr<const MySql::ObjectBase> object, bool toReplace,
			MySql::AsyncSaveCallback callback, MySql::ExceptionCallback except)
			: m_object(STD_MOVE(object)), m_toReplace(toReplace)
			, m_callback(STD_MOVE_IDN(callback)), m_except(STD_MOVE_IDN(except))
		{
		}

	public:
		boost::shared_ptr<const MySql::ObjectBase> getCombinableObject() const OVERRIDE {
			return m_object;
		}
		const char *getTableName() const OVERRIDE {
			return m_object->getTableName();
		}
		void execute(std::string &query, MySql::Connection &conn){
			PROFILE_ME;

			try {
				m_object->syncGenerateSql(query, m_toReplace);
				bool succeeded = false;
				try {
					LOG_POSEIDON_DEBUG("Executing SQL in ", m_object->getTableName(), ": ", query);
					conn.executeSql(query);
					succeeded = true;
				} catch(MySql::Exception &e){
					LOG_POSEIDON_DEBUG("MySql::Exception: errorCode = ", e.errorCode(), ", message = ", e.what());
					if(!m_callback || (e.errorCode() != ER_DUP_ENTRY)){
						throw;
					}
				}

				if(m_callback){
					enqueueJob(boost::make_shared<SaveCallbackJob>(STD_MOVE(m_callback), succeeded, conn.getInsertId(), m_except));
				}
			} catch(...){
				if(m_except){
					m_except();
				}
				throw;
			}
		}
	};

	class LoadOperation : public OperationBase {
	private:
		const boost::shared_ptr<MySql::ObjectBase> m_object;
		const std::string m_query;

		MySql::AsyncLoadCallback m_callback;
		const MySql::ExceptionCallback m_except;

	public:
		LoadOperation(boost::shared_ptr<MySql::ObjectBase> object, std::string query,
			MySql::AsyncLoadCallback callback, MySql::ExceptionCallback except)
			: m_object(STD_MOVE(object)), m_query(STD_MOVE(query))
			, m_callback(STD_MOVE_IDN(callback)), m_except(STD_MOVE_IDN(except))
		{
		}

	public:
		boost::shared_ptr<const MySql::ObjectBase> getCombinableObject() const OVERRIDE {
			return VAL_INIT; // 不能合并。
		}
		const char *getTableName() const OVERRIDE {
			return m_object->getTableName();
		}
		void execute(std::string &query, MySql::Connection &conn){
			PROFILE_ME;

			try {
				query = m_query;

				LOG_POSEIDON_INFO("MySQL load: table = ", m_object->getTableName(), ", query = ", query);
				conn.executeSql(query);
				const bool found = conn.fetchRow();
				if(found){
					m_object->syncFetch(conn);
					m_object->enableAutoSaving();
				}

				if(m_callback){
					enqueueJob(boost::make_shared<LoadCallbackJob>(STD_MOVE(m_callback), found, m_except));
				}
			} catch(...){
				if(m_except){
					m_except();
				}
				throw;
			}
		}
	};

	class BatchLoadOperation : public OperationBase {
	private:
		const char *const m_tableHint;
		boost::shared_ptr<MySql::ObjectBase> (*const m_factory)();
		const std::string m_query;

		MySql::BatchAsyncLoadCallback m_callback;
		const MySql::ExceptionCallback m_except;

	public:
		BatchLoadOperation(const char *tableHint, boost::shared_ptr<MySql::ObjectBase> (*factory)(), std::string query,
			MySql::BatchAsyncLoadCallback callback, MySql::ExceptionCallback except)
			: m_tableHint(tableHint), m_factory(factory), m_query(STD_MOVE(query))
			, m_callback(STD_MOVE_IDN(callback)), m_except(STD_MOVE_IDN(except))
		{
		}

	public:
		boost::shared_ptr<const MySql::ObjectBase> getCombinableObject() const OVERRIDE {
			return VAL_INIT; // 不能合并。
		}
		const char *getTableName() const OVERRIDE {
			return m_tableHint;
		}
		void execute(std::string &query, MySql::Connection &conn){
			PROFILE_ME;

			try {
				query = m_query;

				LOG_POSEIDON_INFO("MySQL batch load: tableHint = ", m_tableHint, "query = ", query);
				conn.executeSql(query);

				std::vector<boost::shared_ptr<MySql::ObjectBase> > objects;
				while(conn.fetchRow()){
					AUTO(object, (*m_factory)());
					object->syncFetch(conn);
					object->enableAutoSaving();
					objects.push_back(STD_MOVE(object));
				}

				if(m_callback){
					enqueueJob(boost::make_shared<BatchLoadCallbackJob>(STD_MOVE(m_callback), STD_MOVE(objects), m_except));
				}
			} catch(...){
				if(m_except){
					m_except();
				}
				throw;
			}
		}
	};

	class Thread : public boost::thread {
	private:
		class WorkingTimeAccumulator : NONCOPYABLE {
		private:
			Thread *const m_owner;
			const char *const m_table;

		public:
			WorkingTimeAccumulator(Thread *owner, const char *table)
				: m_owner(owner), m_table(table)
			{
				m_owner->accumulateTimeForTable("");
			}
			~WorkingTimeAccumulator(){
				m_owner->accumulateTimeForTable(m_table);
			}
		};

		struct TableNameComparator {
			bool operator()(const char *lhs, const char *rhs) const {
				return std::strcmp(lhs, rhs) < 0;
			}
		};

		struct OperationElement {
			boost::uint64_t dueTime;
			boost::shared_ptr<const MySql::ObjectBase> combinableObject;

			boost::shared_ptr<OperationBase> operation;

			OperationElement(boost::uint64_t dueTime_, boost::shared_ptr<OperationBase> operation_)
				: dueTime(dueTime_), combinableObject(operation_->getCombinableObject())
				, operation(STD_MOVE(operation_))
			{
			}
		};

		MULTI_INDEX_MAP(OperationMap, OperationElement,
			MULTI_MEMBER_INDEX(dueTime)
			MULTI_MEMBER_INDEX(combinableObject)
		);

	private:
		const std::size_t m_index;

		volatile bool m_running;

		mutable boost::mutex m_mutex;
		OperationMap m_queue;
		volatile bool m_urgent; // 无视延迟写入，一次性处理队列中所有操作。
		boost::condition_variable m_newAvail;
		boost::condition_variable m_queueEmpty;

		// 性能统计。
		mutable boost::mutex m_profileMutex;
		double m_profileFlushedTime;
		std::map<const char *, double, TableNameComparator> m_profile;

	public:
		explicit Thread(std::size_t index)
			: m_index(index)
			, m_running(true)
			, m_urgent(false)
			, m_profileFlushedTime(getHiResMonoClock())
		{
			boost::thread(&Thread::loop, this).swap(*this);
		}

	private:
		void accumulateTimeForTable(const char *table) NOEXCEPT {
			const AUTO(now, getHiResMonoClock());
			try {
				const boost::mutex::scoped_lock lock(m_profileMutex);
				m_profile[table] += now - m_profileFlushedTime;
			} catch(...){
			}
			m_profileFlushedTime = now;
		}

		void loop();

	public:
		void shutdown(){
			atomicStore(m_running, false, ATOMIC_RELEASE);
		}
		void join(){
			waitTillIdle();
			boost::thread::join();
		}

		std::size_t getProfile(std::vector<MySqlDaemon::SnapshotItem> &ret, unsigned thread) const {
			const boost::mutex::scoped_lock lock(m_profileMutex);
			const std::size_t count = m_profile.size();
			ret.reserve(ret.size() + count);
			for(AUTO(it, m_profile.begin()); it != m_profile.end(); ++it){
				MySqlDaemon::SnapshotItem item;
				item.thread = thread;
				item.table = it->first;
				item.usTotal = static_cast<boost::uint64_t>(it->second * 1.0e6);
				ret.push_back(item);
			}
			return count;
		}

		void addOperation(boost::shared_ptr<OperationBase> operation, bool urgent){
			const AUTO(combinableObject, operation->getCombinableObject());
			AUTO(dueTime, getFastMonoClock());
			// 有紧急操作时无视写入延迟，这个逻辑不在这里处理。
			// if(combinableObject && !urgent){ // 这个看似合理但是实际是错的。
			if(combinableObject || urgent){ // 确保紧急操作排在其他所有操作之后。
				dueTime += g_saveDelay;
			}

			const boost::mutex::scoped_lock lock(m_mutex);
			if(combinableObject){
				AUTO(it, m_queue.find<1>(combinableObject));
				if(it != m_queue.end<1>()){
					m_queue.setKey<1, 0>(it, dueTime);
					goto _inserted;
				}
			}
			m_queue.insert(OperationElement(dueTime, STD_MOVE(operation)));
		_inserted:
			if(urgent){
				atomicStore(m_urgent, true, ATOMIC_RELEASE);
			}
			m_newAvail.notify_all();
		}
		void waitTillIdle(){
			boost::mutex::scoped_lock lock(m_mutex);
			atomicStore(m_urgent, true, ATOMIC_RELEASE); // 在获取互斥锁之前设置紧急状态。
			m_newAvail.notify_all();
			while(!m_queue.empty()){
				LOG_POSEIDON_INFO("There are ", m_queue.size(), " object(s) in my queue.");
				atomicStore(m_urgent, true, ATOMIC_RELEASE);
				m_newAvail.notify_all();
				m_queueEmpty.timed_wait(lock, boost::posix_time::seconds(1));
			}
		}
	};

	void Thread::loop(){
		PROFILE_ME;
		Logger::setThreadTag(" D  "); // Database
		LOG_POSEIDON_INFO("MySQL thread ", m_index, " started.");

		boost::shared_ptr<MySql::Connection> conn;
		std::size_t reconnectDelay = 0;

		boost::shared_ptr<OperationBase> operation;
		std::size_t retryCount = 0;

		for(;;){
			try {
				accumulateTimeForTable("");

				if(!conn){
					LOG_POSEIDON_INFO("Connecting to MySQL server...");

					if(reconnectDelay == 0){
						reconnectDelay = 1;
					} else {
						LOG_POSEIDON_INFO("Will retry after ", reconnectDelay, " milliseconds.");

						boost::mutex::scoped_lock lock(m_mutex);
						m_newAvail.timed_wait(lock,
							boost::posix_time::milliseconds(static_cast<boost::int64_t>(reconnectDelay)));

						reconnectDelay <<= 1;
						if(reconnectDelay > g_maxReconnDelay){
							reconnectDelay = g_maxReconnDelay;
						}
					}
					try {
						conn = MySqlDaemon::createConnection();
					} catch(...){
						if(!atomicLoad(m_running, ATOMIC_ACQUIRE)){
							LOG_POSEIDON_WARNING("Shutting down...");
							goto _exitLoop;
						}
						throw;
					}

					LOG_POSEIDON_INFO("Successfully connected to MySQL server.");
					reconnectDelay = 0;
				}

				if(!operation){
					boost::mutex::scoped_lock lock(m_mutex);
					while(m_queue.empty()){
						m_queueEmpty.notify_all();
						atomicStore(m_urgent, false, ATOMIC_RELEASE);
						accumulateTimeForTable("");

						if(!atomicLoad(m_running, ATOMIC_ACQUIRE)){
							goto _exitLoop;
						}
						m_newAvail.timed_wait(lock, boost::posix_time::seconds(1));
					}
					if(!atomicLoad(m_urgent, ATOMIC_ACQUIRE) && (getFastMonoClock() < m_queue.begin<0>()->dueTime)){
						m_newAvail.timed_wait(lock, boost::posix_time::seconds(1));
						continue;
					}
					operation = m_queue.begin<0>()->operation;
					m_queue.erase(m_queue.begin<0>());
					retryCount = 0;
				}

				unsigned mysqlErrorCode = 99999;
				std::string mysqlErrorMsg;
				std::string query;
				try {
					try {
						const WorkingTimeAccumulator profiler(this, operation->getTableName());
						operation->execute(query, *conn);
					} catch(MySql::Exception &e){
						mysqlErrorCode = e.errorCode();
						mysqlErrorMsg = e.what();
						throw;
					} catch(std::exception &e){
						mysqlErrorMsg = e.what();
						throw;
					} catch(...){
						mysqlErrorMsg = "Unknown exception";
						throw;
					}
				} catch(...){
					if(retryCount < g_maxRetryCount){
						++retryCount;
					} else {
						LOG_POSEIDON_WARNING("Max retry count exceeded. Give up.");
						operation.reset();

						char temp[32];
						unsigned len = (unsigned)std::sprintf(temp, "%05u", mysqlErrorCode);
						std::string dump;
						dump.reserve(1024);
						dump.assign("-- Error code = ");
						dump.append(temp, len);
						dump.append(", Description = ");
						dump.append(mysqlErrorMsg);
						dump.append("\n");
						dump.append(query);
						dump.append(";\n\n");
						{
							const boost::mutex::scoped_lock dumpLock(g_dumpMutex);
							std::size_t total = 0;
							do {
								::ssize_t written = ::write(g_dumpFile.get(), dump.data() + total, dump.size() - total);
								if(written <= 0){
									break;
								}
								total += static_cast<std::size_t>(written);
							} while(total < dump.size());
						}
					}
					throw;
				}
				operation.reset();
			} catch(MySql::Exception &e){
				LOG_POSEIDON_ERROR("MySql::Exception thrown in MySQL daemon: errorCode = ", e.errorCode(), ", what = ", e.what());
				conn.reset();
			} catch(std::exception &e){
				LOG_POSEIDON_ERROR("std::exception thrown in MySQL daemon: what = ", e.what());
				conn.reset();
			} catch(...){
				LOG_POSEIDON_ERROR("Unknown exception thrown in MySQL daemon.");
				conn.reset();
			}
		}
	_exitLoop:
		;

		if(!m_queue.empty()){
			LOG_POSEIDON_WARNING("There are still ", m_queue.size(), " object(s) in MySQL queue");
			m_queue.clear();
		}
		m_queueEmpty.notify_all();

		LOG_POSEIDON_INFO("MySQL thread ", m_index, " stopped.");
	}

	volatile bool g_running = false;
	std::vector<boost::shared_ptr<Thread> > g_threads;

	void commitOperation(const char *table, boost::shared_ptr<OperationBase> operation, bool urgent){
		if(g_threads.empty()){
			DEBUG_THROW(Exception, SharedNts::observe("No MySQL thread is running"));
		}

		std::size_t threadIndex = 0;
		const char *p = table;
		while(*p){
			threadIndex += (unsigned char)*p;
			++p;
		}
		threadIndex %= g_threads.size();

		LOG_POSEIDON_DEBUG("Assigning MySQL table `", table, "` to thread ", threadIndex);
		g_threads.at(threadIndex)->addOperation(STD_MOVE(operation), urgent);
	}
}

void MySqlDaemon::start(){
	if(atomicExchange(g_running, true, ATOMIC_ACQ_REL) != false){
		LOG_POSEIDON_FATAL("Only one daemon is allowed at the same time.");
		std::abort();
	}
	LOG_POSEIDON_INFO("Starting MySQL daemon...");

	AUTO_REF(conf, MainConfig::getConfigFile());

	conf.get(g_serverAddr, "mysql_server_addr");
	LOG_POSEIDON_DEBUG("MySQL server addr = ", g_serverAddr);

	conf.get(g_serverPort, "mysql_server_port");
	LOG_POSEIDON_DEBUG("MySQL server port = ", g_serverPort);

	conf.get(g_username, "mysql_username");
	LOG_POSEIDON_DEBUG("MySQL username = ", g_username);

	conf.get(g_password, "mysql_password");
	LOG_POSEIDON_DEBUG("MySQL password = ", g_password);

	conf.get(g_schema, "mysql_schema");
	LOG_POSEIDON_DEBUG("MySQL schema = ", g_schema);

	conf.get(g_useSsl, "mysql_use_ssl");
	LOG_POSEIDON_DEBUG("MySQL use ssl = ", g_useSsl);

	conf.get(g_charset, "mysql_charset");
	LOG_POSEIDON_DEBUG("MySQL charset = ", g_charset);

	conf.get(g_dumpDir, "mysql_dump_dir");
	LOG_POSEIDON_DEBUG("MySQL dump dir = ", g_dumpDir);

	conf.get(g_maxThreads, "mysql_max_threads");
	LOG_POSEIDON_DEBUG("MySQL max threads = ", g_maxThreads);

	conf.get(g_saveDelay, "mysql_save_delay");
	LOG_POSEIDON_DEBUG("MySQL save delay = ", g_saveDelay);

	conf.get(g_maxReconnDelay, "mysql_max_reconn_delay");
	LOG_POSEIDON_DEBUG("MySQL max reconnect delay = ", g_maxReconnDelay);

	conf.get(g_maxRetryCount, "mysql_max_retry_count");
	LOG_POSEIDON_DEBUG("MySQL max retry count = ", g_maxRetryCount);

	char temp[256];
	unsigned len = formatTime(temp, sizeof(temp), getLocalTime(), false);

	std::string dumpPath;
	dumpPath.assign(g_dumpDir);
	dumpPath.push_back('/');
	dumpPath.append(temp, len);
	dumpPath.append(".log");

	LOG_POSEIDON_INFO("Creating SQL dump file: ", dumpPath);
	if(!g_dumpFile.reset(::open(dumpPath.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL, 0644))){
		const int errCode = errno;
		LOG_POSEIDON_FATAL("Error creating SQL dump file: errno = ", errCode,
			", description = ", getErrorDesc(errCode));
		std::abort();
	}

	g_threads.resize(std::max<std::size_t>(g_maxThreads, 1));
	for(std::size_t i = 0; i < g_threads.size(); ++i){
		LOG_POSEIDON_INFO("Creating MySQL thread ", i);
		g_threads[i] = boost::make_shared<Thread>(i);
	}
}
void MySqlDaemon::stop(){
	if(atomicExchange(g_running, false, ATOMIC_ACQ_REL) == false){
		return;
	}
	LOG_POSEIDON_INFO("Stopping MySQL daemon...");

	for(std::size_t i = 0; i < g_threads.size(); ++i){
		g_threads[i]->shutdown();
	}
	for(std::size_t i = 0; i < g_threads.size(); ++i){
		LOG_POSEIDON_INFO("Stopping MySQL thread ", i);
		if(g_threads[i]->joinable()){
			g_threads[i]->join();
		}
	}
	g_threads.clear();
}

boost::shared_ptr<MySql::Connection> MySqlDaemon::createConnection(){
	return MySql::Connection::create(g_serverAddr, g_serverPort, g_username, g_password, g_schema, g_useSsl, g_charset);
}

std::vector<MySqlDaemon::SnapshotItem> MySqlDaemon::snapshot(){
	std::vector<SnapshotItem> ret;
	for(std::size_t i = 0; i < g_threads.size(); ++i){
		g_threads[i]->getProfile(ret, i);
	}
	return ret;
}

void MySqlDaemon::waitForAllAsyncOperations(){
	for(std::size_t i = 0; i < g_threads.size(); ++i){
		g_threads[i]->waitTillIdle();
	}
}

void MySqlDaemon::enqueueForSaving(boost::shared_ptr<const MySql::ObjectBase> object, bool toReplace, bool urgent,
	MySql::AsyncSaveCallback callback, MySql::ExceptionCallback except)
{
	const AUTO(tableName, object->getTableName());
	const bool reallyUrgent = urgent || callback;
	commitOperation(tableName,
		boost::make_shared<SaveOperation>(
			STD_MOVE(object), toReplace, STD_MOVE(callback), STD_MOVE(except)),
		reallyUrgent);
}
void MySqlDaemon::enqueueForLoading(boost::shared_ptr<MySql::ObjectBase> object, std::string query,
	MySql::AsyncLoadCallback callback, MySql::ExceptionCallback except)
{
	const AUTO(tableName, object->getTableName());
	commitOperation(tableName,
		boost::make_shared<LoadOperation>(
			STD_MOVE(object), STD_MOVE(query), STD_MOVE(callback), STD_MOVE(except)),
		true);
}
void MySqlDaemon::enqueueForBatchLoading(boost::shared_ptr<MySql::ObjectBase> (*factory)(),
	const char *tableHint, std::string query,
	MySql::BatchAsyncLoadCallback callback, MySql::ExceptionCallback except)
{
	commitOperation(tableHint,
		boost::make_shared<BatchLoadOperation>(
			tableHint, factory, STD_MOVE(query), STD_MOVE(callback), STD_MOVE(except)),
		true);
}

}
