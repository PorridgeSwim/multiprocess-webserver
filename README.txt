# Team Flowers

You Zhou (yz3883)
Aoxue Wei (aw3389)
Panyu Gao (pg2676)

# Homework 3

part1: works well

part2: Our solution is working
We used unnamed binary semaphore. sem_wait() may be interrupted, thus we handled EINTR signal.

part3: works well

part5: Our solution is working. 
task2: Two non-thread-safe functions are `strtok` and `inet_ntoa`. 
       The thread-safe version of `strtok` is `strtok_r`, we replace it with `strtok_r`. 
       The thread-safe version of `inet_ntoa` should be `inet_ntoa_r`, but `inet_ntoa_r` is not working in our environment. We replace it with `inet_ntop`. 
We use `pthread_detach(pthread_self())` in this part. If we use `pthread_join()` and a thread terminate before `pthread_join()`, it may remain as a thread zombie for a while which is not expected. 

part6: Our solution is working. 
The `pthread_join()` should be called after all threads are created.  

part7: Our solution is working. 
task1: We use `pthread_cond_signal()` in this part. When there is a socket in the blocking queue, only one thread are able to get the socket and process it no matter how many threads are not in use. So the main thread should call `pthread_cond_signal()`. `pthread_cond_broadcast()` may also works here, but the main thread do not need to do that. 

part8: Our solution is working.

part10 task3:
In this part I set a key to tell the parent process whether to print the stat or not, and this key can only be changed by the handler. Under my design, there is nothing special happen if I kill the child process. Because I didn't put key in the shared memory, child process can't changed the value of key in the parent process.

part12: Our solution is working.
We set the first parameter in waitpid to -1, so the parent process will fork a new child process if any of the child processes is killed.

part13: Our solution is working. 
We pre-fork 4 child-processes and use `socketpair()` to create four connections between parent process and four child processes. 
