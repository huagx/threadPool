#ifndef _THREAD_POOL_H_
#define _THREAD_POOL_H_

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <iostream>
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>

#include <iostream>

const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_IDLE_TIME = 60;

enum class PoolMode
{
	MODE_FIXED,  //固定数量的线程
	MODE_CACHED	 //线程数可动态增长
};

class Thread
{
public:
	using ThreadFunc = std::function<void(int)>;
	Thread(ThreadFunc func)
		: func_(func)
		, threadId_(generateId_++)
	{
	}
	~Thread() = default;
	int getThreadId() const
	{
		return threadId_;
	}
	void start() 
	{
		std::thread t(func_, threadId_);
		t.detach(); 
	}
private:
	ThreadFunc func_;
	static int generateId_; //保存线程id
	int threadId_;
};

class ThreadPool 
{
public:
	ThreadPool()
		:initThreadSize_(std::max(std::thread::hardware_concurrency(), (unsigned int)1)),
		taskSize_(0),
		taskQueMaxthreshHold_(TASK_MAX_THRESHHOLD),
		poolMode_(PoolMode::MODE_FIXED),
		idleThreadNum_(0),
		curThreadNum_(0),
		threadSizeThreshHold_(INT32_MAX),
		start_(false)
	{
	}
	~ThreadPool()
	{
		start_ = false;
		//2、 任务线程先获取到锁，发现任务队列为空阻塞在条件变量上，然后释放锁，
		//主线程此时抢到锁，然后发现线程队列不为空，释放锁阻塞在条件变量上，
		//主线程等待任务线程唤醒exitCond_，而任务线程等待主线程唤醒queueNotEmpty_，故而产生死锁。  
		//所以要在锁抢到之后再唤醒queueNotEmpty_。
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		queueNotEmpty_.notify_all(); //先唤醒阻塞在等待状态上的线程池里面的函数
		//等待线程池线程结束
		exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
		std::cout << "threadPool resource release end !" << std::endl;
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	//设置线程池模式
	void setPoolMode(PoolMode mode)
	{
		if (threadRunState())
			return;
		poolMode_ = mode;
	}

	void setTaskQueMaxThreshHold(int threshhold)
	{
		if (threadRunState())
			return;
		taskQueMaxthreshHold_ = threshhold;
	}

	void setThreadSizeThreshHold(int threshhold) 
	{
		if (threadRunState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED)
		{
			threadSizeThreshHold_ = threshhold;
		}
	}

	//使用可变参模板  Func&& func 引用折叠 既可以传右值引用也可以传左值引用， 
	//返回值 future<> 根据decltype推导出类型
	template<typename Func, typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	{
		using RType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...)
			);
		std::future<RType> result = task->get_future(); 

		//获取锁
		std::unique_lock<std::mutex> lock(taskQueMtx_);

		//等待任务队列有空余

		//用户提交任务，等待时间不能超过1s
		if (!queueNotFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool { return taskQue_.size() < taskQueMaxthreshHold_; }))
		{
			//等待超过1s直接返回。
			std::cerr << "task queue is full, submit task fail." << std::endl;

			//任务提交失败返回一个对象的0值
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType {return RType(); });
			(*task)();
			return task->get_future();
		}

		//任务队列有空余，将任务提交
		// using Task = std::function<void()>;
		taskQue_.emplace([task]() {
			(*task)();
			});

		taskSize_++;

		queueNotEmpty_.notify_all();

		if (poolMode_ == PoolMode::MODE_CACHED
			&& taskSize_ > idleThreadNum_
			&& curThreadNum_ < threadSizeThreshHold_)
		{
			//开始创建新线程
			std::cout << "create New Thread..." << std::endl;
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getThreadId();
			threads_.emplace(ptr->getThreadId(), std::move(ptr));
			threads_[threadId]->start();
			idleThreadNum_++;
			curThreadNum_++;
		}
		return result;
	}

	//开启线程池
	void start(int initThreadNum = std::max(std::thread::hardware_concurrency(), (unsigned int)1))
	{
		initThreadSize_ = initThreadNum;
		curThreadNum_ = initThreadSize_;
		start_ = true;
		for (int i = 0; i < initThreadSize_; i++)
		{
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			threads_.emplace(ptr->getThreadId(), std::move(ptr));
		}

		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start();
			idleThreadNum_++;
		}
	}
private:
	void threadFunc(int thradId)
	{
		auto lastTime = std::chrono::high_resolution_clock().now();
		while (true)
		{
			Task task;
			{
				//获取锁
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid: " << std::this_thread::get_id() << "， 尝试获取任务..." << std::endl;
				//等待notEmpty条件 
				// 1、 主线程析构时先获取到锁
				//防止主线程在析构线程池的时候，获取到锁，
				//此时任务函数刚好处于没有获取到锁的状态，主线程判断线程队列中的线程的个数不为0，
				//然后将锁释放，阻塞在exitCond_条件变量上，此时任务线程抢到锁，然后发现任务队列中的任务个数为0，
				//然后等待在queueNotEmpty_条件变量上，此时没有人会唤醒queueNotEmpty_和exitCond_条件变量，导致死锁。
				//锁加双重判断  start_ 防止了该现象

				while (taskQue_.size() == 0)
				{
					if (!start_) //等待所有任务执行完成之后再释放线程池
					{
						//工作完成之后释放线程
						threads_.erase(thradId);
						exitCond_.notify_all();
						std::cout << "destory work thread, Id: " << std::this_thread::get_id() << " exit!" << std::endl;
					}

					if (poolMode_ == PoolMode::MODE_CACHED)
					{
						if (std::cv_status::timeout == queueNotEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadNum_ > initThreadSize_)
							{
								//开始回收线程
								//记录线程数量的值改变
								//把线程对象从线程列表容器中删除
								threads_.erase(thradId);
								curThreadNum_--;
								idleThreadNum_--;
								std::cout << "destory thread, Id: " << std::this_thread::get_id() << " exit!" << std::endl;
								return;
							}
						}
					}
					else
					{
						queueNotEmpty_.wait(lock);
					}
				}

				idleThreadNum_--;

				//从任务队列中取一个任务出来
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;
				//std::cout << "tid: " << std::this_thread::get_id() << "， 获取任务成功..." << std::endl;
				//如果此时还有其他任务，继续通知其他线程执行任务
				if (taskQue_.size() > 0)
				{
					queueNotEmpty_.notify_all();
				}
				//任务队列不为满，可以提交任务了
				queueNotFull_.notify_all();
			}
			//执行task任务
			task();
			idleThreadNum_++;
			lastTime = std::chrono::high_resolution_clock().now();
		}
	}
	bool threadRunState() const
	{
		return start_;
	}
private:
	//std::vector<std::unique_ptr<Thread>> threads_;
	std::unordered_map<int,std::unique_ptr<Thread>> threads_;
	unsigned int initThreadSize_;
	std::atomic_uint curThreadNum_;
	size_t threadSizeThreshHold_; //线程数量阈值

	using Task = std::function<void()>;
	std::queue<Task> taskQue_; //任务队列
	std::atomic_uint taskSize_; //任务数量
	int taskQueMaxthreshHold_; //任务队列上限阈值
	
	std::mutex taskQueMtx_;
	std::condition_variable queueNotFull_;
	std::condition_variable queueNotEmpty_;
	std::condition_variable exitCond_;
	PoolMode poolMode_;
	std::atomic_bool start_{false}; 

	//记录空闲线程的任务数量
	std::atomic_uint idleThreadNum_;
};

int Thread::generateId_ = 0;
#endif 
