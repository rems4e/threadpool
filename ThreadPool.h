//
//  ThreadPool.h
//  ThreadPool
//
//  Created by Rémi Saurel on 2014-06-27.
//

#ifndef Petri_ThreadPool_h
#define Petri_ThreadPool_h

#include "Callable.h"
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace ThreadPool {

    template <typename CallableType>
    auto make_callable(CallableType &&c) {
        return Callable<CallableType, std::result_of_t<CallableType()>>(c);
    }

    class TimeoutException : public std::exception {
        using std::exception::exception;
    };

    template <typename _ReturnType>
    class ThreadPool {
        using ReturnType = _ReturnType;

        struct TaskManager {
            // Defined to char if ReturnType is void, so that we can nevertheless create a member
            // variable of this type
            using VoidProofReturnType =
            typename std::conditional<std::is_same<ReturnType, void>::value, char, ReturnType>::type;

            TaskManager(std::unique_ptr<CallableBase<ReturnType>> task)
                    : _task(std::move(task)) {}

            ReturnType returnValue() {
                this->waitForCompletion();

                // No value if void
                return ReturnType(_res);
            }

            ReturnType returnValue(std::chrono::milliseconds timeout) {
                this->waitForCompletion(timeout);

                // No value if void
                return ReturnType(_res);
            }

            void waitForCompletion(std::chrono::milliseconds timeout) {
                std::unique_lock<std::mutex> lk(_mut);
                if(!_cv.wait_for(lk, timeout, [this]() { return _valOK == true; })) {
                    throw TimeoutException();
                }
            }

            void waitForCompletion() {
                std::unique_lock<std::mutex> lk(_mut);
                _cv.wait(lk, [this]() { return _valOK == true; });
            }

            // Void version, simply executes the task
            template <typename _Helper = void>
            std::enable_if_t<(std::is_void<_Helper>::value, std::is_void<ReturnType>::value), void> execute() {
                _task->operator()();
                this->signalCompletion();
            }

            // Non-void version, executes the task and stores it in _res
            template <typename _Helper = void>
            std::enable_if_t<(std::is_void<_Helper>::value, !std::is_void<ReturnType>::value), void> execute() {
                _res = std::move(_task->operator()());
                this->signalCompletion();
            }

            // Signals the completion of the task to _cv, usually to the thread which called
            // waitForCompletion()
            void signalCompletion() {
                _valOK = true;
                _cv.notify_all();
            }

            std::condition_variable _cv;
            std::mutex _mut;
            std::atomic_bool _valOK = {false};

            VoidProofReturnType _res;
            std::unique_ptr<CallableBase<ReturnType>> _task;
        };

    public:
        class TaskResult {
            friend class ThreadPool;

        public:
            TaskResult() = default;

            /**
             * Gets the return value of the task, blocks the calling thread until the result is made
             * available.
             * Not available for ResultType == void specialization
             * @return The return value associated to the task and computed by the worker thread
             */
            template <typename _Helper = void>
            std::enable_if_t<(std::is_void<_Helper>::value, !std::is_void<ReturnType>::value), ReturnType>
            returnValue() {
                return _proxy ? _proxy->returnValue() :
                                throw std::runtime_error("Proxy not associated with a task!");
            }

            /**
             * Blocks the calling thread until the task is complete and the result is available (no
             * result for tasks returning void).
             */
            void waitForCompletion(std::chrono::milliseconds timeout) {
                _proxy ? _proxy->waitForCompletion(timeout) :
                         throw std::runtime_error("Proxy not associated with a task!");
            }

            /**
             * Blocks the calling thread until the task is complete and the result is available (no
             * result for tasks returning void).
             */
            void waitForCompletion() {
                _proxy ? _proxy->waitForCompletion() :
                         throw std::runtime_error("Proxy not associated with a task!");
            }

            /**
             * Checks whether the task result is available.
             * @return Availability of the task result
             */
            bool available() {
                return _proxy ? static_cast<bool>(_proxy->_valOK) : false;
            }

        private:
            std::shared_ptr<TaskManager> _proxy;
        };

    public:
        /**
         * Creates the thread pool.
         * @param capacity Max number of concurrent task at a given time
         * @param name     This string is used for debug purposes: it gives a name to each worker
         *                 threads, allowing for fast thread discimination when run through a
         * debugger
         */
        ThreadPool(std::size_t capacity, std::string const &name = "")
                : _workerThreads(capacity)
                , _threadsCount(capacity)
                , _name(name) {
            int count = 0;
            for(auto &t : _workerThreads) {
                t = std::thread(&ThreadPool::work, this, _name + "_worker " + std::to_string(count++));
            }
        }

        ~ThreadPool() {
            if(_pendingTasks > 0) {
                std::cerr << "Some tasks are still running (" << _pendingTasks << ")!" << std::endl;
            }
            if(_alive) {
                std::cerr << "Thread pool is still alive!" << std::endl;
            }

            this->stop();
        }

        /**
         * Returns the worker threads count, i.e. the max number of concurrent tasks at a given
         * time.
         * @return The current worker threads count
         */
        std::size_t threadCount() const {
            return _threadsCount;
        }

        /**
         * Increments the worker threads count, i.e. allows one more concurrent task to run.
         */
        void addThreadWithinMaxValue(size_t maxValue) {
            if(_alive) {
                std::lock_guard<std::mutex> lock{_threadsMutex};
                if(_workerThreads.size() < maxValue) {
                    _workerThreads.emplace_back(&ThreadPool::work,
                                                this,
                                                _name + "_worker " + std::to_string(_workerThreads.size()));
                    ++_threadsCount;
                }
            }
        }

        /**
         * Pauses the calling thread until there is no more pending tasks.
         */
        void join() {
            if(_alive) {
                // Quick and dirty…
                while(_pendingTasks > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                this->stop();
            }
        }

        /**
         * Clears all of the pending tasks and shuts down all the working threads.
         * The thread pool will be ineffective after that.
         * This is a blocking call.
         */
        void stop() {
            _alive = false;
            std::lock_guard<std::mutex> lock{_threadsMutex};
            _taskAvailable.notify_all();

            for(auto &t : _workerThreads) {
                if(t.joinable() && t.get_id() != std::this_thread::get_id()) {
                    t.join();
                }
            }
        }

        /**
         * Pauses the execution of the thread pool. The tasks that were already running are still
         * executed,
         * but the current and future pending tasks will remain pending until the resume() method is
         * called.
         * If the thread pool is not alive, this is a no-op.
         */
        void pause() {
            if(_alive) {
                _pause = true;
            }
        }

        /**
         * Resumes the execution of the thread pool. If it wasn't alive and paused before, this is a
         * no-op.
         */
        void resume() {
            if(_alive) {
                bool d = true;
                if(_pause.compare_exchange_strong(d, false)) {
                    _taskAvailable.notify_all();
                }
            }
        }

        /**
         * Whether the thread pool is paused.
         * @return true if the thread pool is paused, false otherwise.
         */
        bool is_paused() {
            return _pause;
        }

        /**
         * Adds a task to the thread pool.
         * @param task The task to be addes.
         * @return A proxy object allowing the user to wait for the task completion, query the task
         * completion status and get the task return value
         */
        TaskResult addTask(CallableBase<ReturnType> const &task) {
            TaskResult result;
            // task must be kept alive until execution finishes
            result._proxy = std::make_shared<TaskManager>(task.copy_ptr());

            std::lock_guard<std::mutex> lk(_availabilityMutex);
            ++_pendingTasks;
            _taskQueue.push(result._proxy);
            _taskAvailable.notify_one();

            return result;
        }

    private:
        void work(std::string const &name) {
            while(_alive) {
                std::unique_lock<std::mutex> lk(_availabilityMutex);
                _taskAvailable.wait(lk,
                                    [this]() { return (!_taskQueue.empty() && !_pause) || !_alive; });

                if(!_alive) {
                    return;
                }

                auto taskManager = std::move(_taskQueue.front());
                _taskQueue.pop();

                lk.unlock();

                taskManager->execute();

                --_pendingTasks;
            }
        }

        std::queue<std::shared_ptr<TaskManager>> _taskQueue;
        std::condition_variable _taskAvailable;
        std::mutex _availabilityMutex;

        std::atomic_bool _pause = {false};
        std::atomic_bool _alive = {true};
        std::atomic_uint _pendingTasks = {0};
        std::vector<std::thread> _workerThreads;
        std::atomic<std::size_t> _threadsCount;
        std::mutex _threadsMutex;
        std::string const _name;
    };
}

#endif
