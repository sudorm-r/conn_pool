#ifndef CONNECTION_H
#define CONNECTION_H

/*
	连接状态
*/
enum ConnState {
	INVALID,// 无效	
	READY,	// 就绪
	BUSY	// 忙
};


class ConnectionPool;
/*
	对Socket的封装,抽象成一个“连接”。
*/
class Connection {
public:
	friend class ConnectionPool;
	Connection(ConnectionPool* pool_inst);
	~Connection();

	void init(int fd);
	void close();

	int read(char* buff, int size) {
		// 利用socket_fd进行读数据
		// ……
	}
	int write(const char* buff, int size) {
		// 利用socket_fd进行谢数据
		// ……
	}
private:
	ConnectionPool* conn_pool;
	ConnState state;
	int socket_fd;
	pthread_rwlock_t rwlock;
};

#endif
