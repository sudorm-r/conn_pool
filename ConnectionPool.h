#ifndef CONNECTION_POOL_H
#define CONNECTION_POOL_H
#include <string>
#include <vector>
#include <list>
/*
#include ...	// 其他相关头文件
*/

#define CONN_POOL_INIT_SIZE	10	// 连接池初始连接数
#define CONN_POOL_MAX_SIZE	20	// 连接池最大连接数
#define RECONNECT_COUNT		3	// 连接失效后重连尝试次数
#define RECONNECT_INTERVAL	3	// 重连间隔时间，秒

class Connection;

/*
	连接池，主要功能：
	1. 池中维持最小数量个连接，在需要时扩增，在达到最大数量时等待
	2. 连接在close后自动加入到连接池中
	3. 在连接失效时会自动重连
*/
class ConnectionPool {
public:
	ConnectionPool(const std::string& host, int port, int size = CONN_POOL_INIT_SIZE, int max_size = CONN_POOL_MAX_SIZE);
	~ConnectionPool();

	void init();
	Connection* getConnection();
	void pushConnection(Connection* conn);
private:
	Connection* newConnection();
	void* watch();
	void reconnect();
private:
	std::string host;
	int port;
	int size;
	int max_size;
	std::vector<Connection*> pool;
	std::list<Connection*> ready_pool;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int epollfd;
	bool running;
};

#endif
