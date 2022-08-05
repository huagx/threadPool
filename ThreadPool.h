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
	MODE_FIXED,  //�̶��������߳�
	MODE_CACHED	 //�߳����ɶ�̬����
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
	static int generateId_; //�����߳�id
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
		//2�� �����߳��Ȼ�ȡ�����������������Ϊ�����������������ϣ�Ȼ���ͷ�����
		//���̴߳�ʱ��������Ȼ�����̶߳��в�Ϊ�գ��ͷ������������������ϣ�
		//���̵߳ȴ������̻߳���exitCond_���������̵߳ȴ����̻߳���queueNotEmpty_���ʶ�����������  
		//����Ҫ��������֮���ٻ���queueNotEmpty_��
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		queueNotEmpty_.notify_all(); //�Ȼ��������ڵȴ�״̬�ϵ��̳߳�����ĺ���
		//�ȴ��̳߳��߳̽���
		exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
		std::cout << "threadPool resource release end !" << std::endl;
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	//�����̳߳�ģʽ
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

	//ʹ�ÿɱ��ģ��  Func&& func �����۵� �ȿ��Դ���ֵ����Ҳ���Դ���ֵ���ã� 
	//����ֵ future<> ����decltype�Ƶ�������
	template<typename Func, typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	{
		using RType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...)
			);
		std::future<RType> result = task->get_future(); 

		//��ȡ��
		std::unique_lock<std::mutex> lock(taskQueMtx_);

		//�ȴ���������п���

		//�û��ύ���񣬵ȴ�ʱ�䲻�ܳ���1s
		if (!queueNotFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool { return taskQue_.size() < taskQueMaxthreshHold_; }))
		{
			//�ȴ�����1sֱ�ӷ��ء�
			std::cerr << "task queue is full, submit task fail." << std::endl;

			//�����ύʧ�ܷ���һ�������0ֵ
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType {return RType(); });
			(*task)();
			return task->get_future();
		}

		//��������п��࣬�������ύ
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
			//��ʼ�������߳�
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

	//�����̳߳�
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
				//��ȡ��
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid: " << std::this_thread::get_id() << "�� ���Ի�ȡ����..." << std::endl;
				//�ȴ�notEmpty���� 
				// 1�� ���߳�����ʱ�Ȼ�ȡ����
				//��ֹ���߳��������̳߳ص�ʱ�򣬻�ȡ������
				//��ʱ�������պô���û�л�ȡ������״̬�����߳��ж��̶߳����е��̵߳ĸ�����Ϊ0��
				//Ȼ�����ͷţ�������exitCond_���������ϣ���ʱ�����߳���������Ȼ������������е��������Ϊ0��
				//Ȼ��ȴ���queueNotEmpty_���������ϣ���ʱû���˻ỽ��queueNotEmpty_��exitCond_��������������������
				//����˫���ж�  start_ ��ֹ�˸�����

				while (taskQue_.size() == 0)
				{
					if (!start_) //�ȴ���������ִ�����֮�����ͷ��̳߳�
					{
						//�������֮���ͷ��߳�
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
								//��ʼ�����߳�
								//��¼�߳�������ֵ�ı�
								//���̶߳�����߳��б�������ɾ��
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

				//�����������ȡһ���������
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;
				//std::cout << "tid: " << std::this_thread::get_id() << "�� ��ȡ����ɹ�..." << std::endl;
				//�����ʱ�����������񣬼���֪ͨ�����߳�ִ������
				if (taskQue_.size() > 0)
				{
					queueNotEmpty_.notify_all();
				}
				//������в�Ϊ���������ύ������
				queueNotFull_.notify_all();
			}
			//ִ��task����
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
	size_t threadSizeThreshHold_; //�߳�������ֵ

	using Task = std::function<void()>;
	std::queue<Task> taskQue_; //�������
	std::atomic_uint taskSize_; //��������
	int taskQueMaxthreshHold_; //�������������ֵ
	
	std::mutex taskQueMtx_;
	std::condition_variable queueNotFull_;
	std::condition_variable queueNotEmpty_;
	std::condition_variable exitCond_;
	PoolMode poolMode_;
	std::atomic_bool start_{false}; 

	//��¼�����̵߳���������
	std::atomic_uint idleThreadNum_;
};

int Thread::generateId_ = 0;
#endif 
