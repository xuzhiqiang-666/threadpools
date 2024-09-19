#include"threadpool.h"
#include<thread>
#include<iostream>

const int TASK_MAX_THRESHHOLD = INT32_MAX; //任务队列最大任务阈值
const int THREAD_MAX_THRESHHOLD = 100; // 线程最大线程数
const int THREAD_MAX_IDLE_TIME = 10; // 线程的最大空闲时间 单位：s

// 线程池构造
ThreadPool::ThreadPool()
	:init_threadSize_(0)
	,taskSize_(0)
	, idleThreadSize_(0)
	, curThreadSize_(0)
	,taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
	,threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
	,poolMode_(PoolMode::MODE_FIXED)
	,isPoolRunning_(false)
{}

// 线程池析构
ThreadPool::~ThreadPool()
{
	std::cout << "~ThreadPool()" << std::endl;
	isPoolRunning_ = false;
	
	// 等待线程池里面所有的线程返回 有两种状态：阻塞 & 正在执行任务中
	std::unique_lock<std::mutex> lock(taskQueMtx_);
	notEmpty_.notify_all(); // 激活所有在等待的线程

	exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });

	std::cout << "~ThreadPool end !!!" << std::endl;
}

// 设置线程池的工作模式
void ThreadPool::setMode(PoolMode mode)
{
	if (checkRunningState()) // 正在运行就不能设置
		return;
	poolMode_ = mode;
}


// 设置task任务队列上线阈值
void ThreadPool::setTaskQueMaxThreshHold(int threshhold)
{
	if (checkRunningState())
		return;
	taskQueMaxThreshHold_ = threshhold;
}

// 设置线程池cached模式下线程阈值
void ThreadPool::setThreadSizeThreshHold(int threshhold)
{
	if (checkRunningState())
		return;
	if (poolMode_ == PoolMode::MODE_CACHED)
	{
		threadSizeThreshHold_ = threshhold;
	}
	
}

// 给线程池提交任务
Result ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
	// 获取锁
	std::unique_lock<std::mutex> lock(taskQueMtx_);

	// 线程的通信 等待任务队列有空余
	/*while (taskQue_.size() == taskQueMaxThreshHold_)
	{
		notFull_.wait(lock);
	}*/
	// 是上面的简便实现 wait wait_for wait_until
	// 用户提交任务，最长不能阻塞超过1s否则判断提交任务失败，返回 
	if(!notFull_.wait_for(lock, std::chrono::seconds(1),
		[&]()->bool {return taskQue_.size() < (size_t)taskQueMaxThreshHold_; })) // 如果满足taskQue_.size() < taskQueMaxThreshHold_就说明可以继续执行
	{
		// 表示notFull_等待1s，条件依然没有满足
		std::cerr << "task queue is full, submit task fail." << std::endl;
		return Result(sp,false); // Task 
	}
	// 如果有空余，把任务放入任务队列
	taskQue_.emplace(sp);
	taskSize_++;

	// 因为新放了任务，任务队列肯定不空了 ,notEmpty_通知
	notEmpty_.notify_all();

	// cached模式：任务处理比较紧急 场景：小而块的任务 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来
	if (poolMode_ == PoolMode::MODE_CACHED
		&& taskSize_ > idleThreadSize_
		&& curThreadSize_ < threadSizeThreshHold_)
	{
		std::cout << ">>>create new thread<<<" << std::this_thread::get_id() << std::endl;
		// 创建新线程
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
		int threadId = ptr->getId();
		threads_.emplace(threadId, std::move(ptr));
		// 线程启动
		threads_[threadId]->start();
		// 修改线程个数相关变量
		curThreadSize_++;
		idleThreadSize_++;
	}

	// 返回任务的Result对象
	return Result(sp);
}

// 开启线程池
void ThreadPool::start(int initThreadSize)
{
	// 设置线程池的启动状态
	isPoolRunning_ = true;

	// 记录初始线程个数
	init_threadSize_ = initThreadSize;
	curThreadSize_ = initThreadSize;

	// 创建线程对象
	for (int i = 0; i < init_threadSize_; i++)
	{
		// 创建thread线程对象的时候，把线程函数给到thread线程对象
		//threads_.emplace_back(new Thread(std::bind(&ThreadPool::threadFunc,this)));
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this,std::placeholders::_1));
		int threadId = ptr->getId();
		threads_.emplace(threadId,std::move(ptr));
		//threads_.emplace_back(std::move(ptr));
	}

	// 启动所有线程 std::vector<Thread*> threads_;
	for (int i = 0; i < init_threadSize_; i++)
	{
		threads_[i]->start(); // 需要去执行一个线程函数
		idleThreadSize_++;
	}
}

// 定义线程函数
void ThreadPool::threadFunc(int threadid) // 线程函数返回，相应的线程也就结束了
{
	auto lastTime = std::chrono::high_resolution_clock().now(); // 获取当前时间
	// 只有当任务执行完才可以结束线程
	while (1)
	{
		std::shared_ptr<Task> task;

		{
			// 先获取锁
			std::unique_lock<std::mutex> lock(taskQueMtx_);

			std::cout << "tid" << std::this_thread::get_id() << "尝试获取任务..." << std::endl;

			// 每一秒中返回一次 怎么区分：超时返回？还是有任务待执行返回
			while (taskQue_.size() == 0)
			{
				if (!isPoolRunning_)
				{
					// 执行完任务发现线程池结束
					threads_.erase(threadid);
					std::cout << "tid" << std::this_thread::get_id() << "exit" << std::endl;
					exitCond_.notify_all();
					return;
				}

				// cached模式下，有可能已经创建了很多线程，但是空闲时间超过60s,应该把多余的线程
				// 结束回收掉 (超过initThreadSize_数量的线程要进行回收)
				// 当前时间 - 上一次线程执行的时间>60s
				if (poolMode_ == PoolMode::MODE_CACHED)
				{
					// 条件变量，超时返回
					if (std::cv_status::timeout ==
						notEmpty_.wait_for(lock, std::chrono::seconds(1)))
					{
						auto now = std::chrono::high_resolution_clock().now();
						auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime); // 计算线程空闲时间
						if (dur.count() >= THREAD_MAX_IDLE_TIME
							&& curThreadSize_ > init_threadSize_)
						{
							// 开始回收当前线程
							// 记录线程数量的相关变量的值修改
							// 把线程对象从线程列表容器中删除
							threads_.erase(threadid);
							curThreadSize_--;
							idleThreadSize_--;

							std::cout << "tid" << std::this_thread::get_id() << "exit" << std::endl;
							return;
						}
					}
				}
				else
				{
					// 等待notEmpty条件
					notEmpty_.wait(lock);
				}
			}

			
			idleThreadSize_--;
			
			std::cout << "tid" << std::this_thread::get_id() << "获取任务成功..." << std::endl;
			// 从任务队列中取一个任务出来
			task = taskQue_.front();
			taskQue_.pop();
			taskSize_--;

			// 如果依然有剩余任务，继续通知其他的线程执行程序
			if (taskQue_.size() > 0)
			{
				notEmpty_.notify_all();
			}

			// 取出一个任务进行通知可以继续提交生产任务
			notFull_.notify_all();
		} // 释放锁
		
		// 当前线程负责执行这个任务
		if (task != nullptr)
		{
			//task->run();
			task->exec();
		}
		idleThreadSize_++; // 空闲线程++
		lastTime = std::chrono::high_resolution_clock().now(); // 更新线程执行完任务的时间
	}
	
}

// 检查pool的运行状态
bool ThreadPool::checkRunningState() const
{
	return isPoolRunning_;
}

///////////////////////////////////////////// 线程方法实现

int Thread::generateId_ = 0;

// 线程构造
Thread::Thread(ThreadFunc func)
	:func_(func),threadId_(generateId_++)
{

}

// 线程析构
Thread::~Thread(){}

// 启动线程
void Thread::start()
{
	// 创建一个线程执行一个线程函数
	std::thread t(func_, threadId_); // c++来说线程对象t 和线程函数func_
	t.detach(); //  设置分离线程 把线程对象t和线程函数执行分离开
}

// 获取线程id
int Thread::getId()const
{
	return threadId_;
}


///////////////////////////////////////////// Task方法实现
Task::Task()
	:result_(nullptr)
{}
void Task::exec()
{
	if (result_ != nullptr)
	{
		result_->setVal(run()); // 这里发生动态多态
	}
} 

void Task::setResult(Result* res)
{
	result_ = res;
}


///////////////////////////////////////////// Result方法的实现

Result::Result(std::shared_ptr<Task> task, bool isValid)
	: isValid_(isValid)
	, task_(task)
{
	task_->setResult(this);
}

// 问题二：get方法，用户调用这个方法获取task的返回值
Any Result::get()
{
	if (!isValid_)
	{
		return "";
	}
	sem_.wait(); // task任务如果没有执行完，这里会阻塞用户线程
	return std::move(any_);
}

// 问题一 ：setVal方法，获取任务执行完的返回值
void Result::setVal(Any any)
{
	this->any_ = std::move(any);
	sem_.post(); // 已经获取了任务的返回值，增加信号量资源
}