#include "Connection.h"
#include "ConnectionPool.h"

Connection::Connection(ConnectionPool* pool)
: conn_pool(pool)
, state(ConnState::INVALID) {

}

Connection::~Connection() {
	pthread_rwlock_destroy(&rwlock);
}

void Connection::init(int fd) {
	socket_fd = fd;
	pthread_rwlock_init(&rwlock, NULL);
	state = ConnState::READY;
}

void Connection::close() {
	// "关闭"连接，并不销毁，仅重新加入到连接池
	conn_pool.pushConnection(this);
}
