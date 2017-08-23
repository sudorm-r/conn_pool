#include "ConnectionPool.h"
#include "Connection.h"
#include <functional>
#include <algorithm>

ConnectionPool::ConnectionPool(const std::string& host, int port, int size, int max_size)
: host(host)
, port(port)
, size(size)
, max_size(max_size)
, running(true) {
	pthread_mutex_init(&mutex, NULL);
	pthread_cond_init(&cond, NULL);
	pool.reserve(max_size);
}

ConnectionPool::~ConnectionPool() {
	// 关闭池中所有连接
	pthread_mutex_lock(&mutex);
	for (Connection* conn : pool) {
		pthread_rwlock_wrlock(&conn->rwlock);
		// 删除每个连接的epoll事件
		epoll_ctl(epollfd, EPOLL_CTL_DEL, conn->socket_fd, NULL);
		// 关闭该连接
		close(conn->socket_fd);
		conn->state = ConnState::INVALID;
		pthread_rwlock_unlock(&conn->rwlock);
		pthread_rwlock_destroy(&conn->rwlock);
		delete conn;
	}
	pool.clear();
	ready_pool.clear();
	pthread_mutex_unlock(&mutex);
	pthread_mutex_destroy(&mutex);
	pthread_cond_destroy(&cond);
	close(epollfd);
	running = false;
}

void ConnectionPool::init() {
	// 预先创建size个连接，加入连接池
	for (int i = 0; i < size; i++) {
		Connection* conn = newConnection();
		if (conn == NULL) {
			throw std::exception("init connection pool failed!");
		}
		pool.push_back(conn);
		ready_pool.push_back(conn);
	}
	epollfd = epoll_create(max_size);

	// 创建监视线程
	pthread_t watcher;
	pthread_attr_t watcher_attr;
	pthread_attr_init(&watcher_attr);
	// 以分离状态运行
	pthread_attr_setdetachstate(&watcher_attr, PTHREAD_CREATE_DETACHED);
	pthread_create(&watcher, &watcher_attr, std::bind(&ConnectionPool::watch, this), NULL);
	pthread_attr_destroy(&watcher_attr);
}

Connection* ConnectionPool::newConnection() {
	// 创建一个到服务器的连接并用此socket初始化Connection
	// 此方法是同步的
	if ((fd = socket(AF_INET, SOCK_STREAM, 0) == -1) {
		printf("create socket failed: %s", strerror(errno));
		return NULL;
	}

	struct socketaddr_in server;
	server.sin_family = AF_INET;
	server.sin_port = htons(port);
	if (inet_aton(host.c_str(), &server.sin_addr) == 0) {
		printf("host name is invalid: %s", strerror(errno));
		return NULL;
	}

	// connect to server...
	if (connect(fd, (struct sockaddr*)&server, sizeof(server)) == -1) {
		printf("connect to server failed: %s", strerror(errno));
		return NULL;
	}

	Connection* conn = new (std::nothrow) Connection(this);
	if (conn == NULL) {
		close(fd);
		return NULL;
	}

	// 添加可读监听，在有可读事件时读到0个字节数据，那么此连接便是无效的
	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.ptr = (void*)conn;
	epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

	conn->init(fd);

	return conn;
}

Connection* ConnectionPool::getConnection() {
	pthread_mutex_lock(&mutex);

	// 如果就绪池中无可用连接并且不能创建新的连接，那么等待
	// 此处可加入超时机制
	while (ready_pool.empty() && pool.size() == max_size) {
		pthread_cond_wait(&cond, &mutex);
	}

	// 就绪池没有可用连接，但是此时可以创建新的
	if (ready_pool.empty()) {
		Connection* conn = newConnection();
		if (conn == NULL) {
			pthread_mutex_unlock(&mutex);
			return NULL;
		}
		pool.push_back(conn);
		pthread_mutex_unlock(&mutex);
		return conn;
	}
	else {	// 就绪池有可用连接，取出
		Connection* conn = ready_pool.front();
		ready_pool.pop_front();
		pthread_mutex_unlock(&mutex);
		return conn;
	}

	pthread_mutex_unlock(&mutex);
	return NULL;
}

void ConnectionPool::pushConnection(Connection* conn) {
	// 连接“关闭”后重新加入就绪池，等待被复用
	pthread_mutex_lock(&mutex);
	ready_pool.push_back(conn);
	pthread_cond_broadcast(&cond);	// 通知：有连接可用
	pthread_mutex_unlock(&mutex);
}

void* ConnectionPool::watch() {
	/* 以下未考虑错误 */
	
	// 监视池子中的连接，若发现有失效的就开始重连
	// 这个方法作为线程函数以分离状态在连接池初始化时运行
	struct epoll_event events[max_size];
	while (running) {
		int nfds = epoll_wait(epfd, events, max_size, -1);
		if (nfds == -1)
			continue;
		Connection *conn = NULL;
		for (int i = 0; i < nfds; i++) {
			conn = (Connection*)events[i].data.ptr;
			if (events[i].events & EPOLLIN) {
				int length;
				ioctl(conn->socket_fd, FIONREAD, &length);
				if (length == 0) {
					// 读到0个字节数据，即此链接失效
					pthread_rwlock_wrlock(&conn->rwlock);
					conn->state = ConnState::INVALID;
					pthread_rwlock_unlock(&conn->rwlock);
					// 删除此失效连接的epoll监听
					epoll_ctl(epollfd, EPOLL_CTL_DEL, conn->socket_fd, NULL);
					// 如果此连接在池中的话先暂时移除之
					pool.erase(std::find(pool.begin(), pool.end(), conn));
					ready_pool.erase(std::find(ready_pool.begin(), ready_pool.end(), conn));
					// 假设有一线程池，将重连工作扔给线程池
					ThreadPool::addTask(std::bind(&ConnectionPool::reconnect, this, conn));
				}
			}
		}
	}
}

void ConnectionPool::reconnect(Connection* conn) {
	// 线程函数，重连此连接
	close(conn->socket_fd);
	for (int i = 0; i < RECONNECT_COUNT; i++) {
		conn->socket_fd = socket(AF_INET, SOCK_STREAM, 0);

		struct sockaddr_in server;
		server.sin_family = AF_INET;
		server.sin_port = htons(port);
		inet_aton(host.c_str(), &server.sin_addr);

		if (connect(conn->socket_fd, (struct sockaddr*)&server, sizeof(server)) == -1) {
			close(conn->socket_fd);
			usleep(RECONNECT_INTERVAL * 1000);
			continue;
		}

		// 重连成功，重置状态
		pthread_rwlock_wrlock(&conn->rwlock);
		conn->state = ConnState::READY;
		pthread_rwlock_unlock(&conn->rwlock);

		// 重新添加epoll监听
		struct epoll_event event;
		event.events = EPOLLIN;
		event.data.ptr = (void*)conn;
		epoll_ctl(epollfd, EPOLL_CTL_ADD, conn->socket_fd, &event);

		// 重新加入池中
		pthread_mutex_lock(&mutex);
		pool.push_back(conn);
		ready_pool.push_back(conn);
		pthread_mutex_unlock(&mutex);

		return;	// 结束
	}
	
	// 经过最大次数的重连依然失败，那么放弃此连接。（已从池中移除）
}
