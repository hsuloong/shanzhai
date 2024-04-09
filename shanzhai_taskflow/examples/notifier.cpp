/*
 * Copyright 2024. All rights reserved.
 * Author: hsuloong@outlook.com
 * Created on: 2024.03.11
 */

#include "taskflow/core/notifier.hpp"

#include <iostream>
#include <thread>

bool predicate = false;

/*
Output:
ThreadFunc1-2
ThreadFunc1-3
ThreadFunc1-4
ThreadFunc1-7
.......wait 5 seconds
ThreadFunc2-1
ThreadFunc1-8
ThreadFunc1-9
.......wait 5 seconds
ThreadFunc2-2
*/

void ThreadFunc1(::shanzhai_tf::Notifier *notifier) {
  if (predicate) {
    std::cout << "ThreadFunc1-1\n";
  }
  std::cout << "ThreadFunc1-2\n";
  auto w = notifier->GetWaiter(0);
  std::cout << "ThreadFunc1-3\n";
  notifier->PrepareWait(w);
  std::cout << "ThreadFunc1-4\n";
  if (predicate) {
    std::cout << "ThreadFunc1-5\n";
    notifier->CancelWait(w);
    std::cout << "ThreadFunc1-6\n";
  }
  std::cout << "ThreadFunc1-7\n";
  notifier->CommitWait(w);
  if (predicate) {
    std::cout << "ThreadFunc1-8\n";
  }
  std::cout << "ThreadFunc1-9\n";
}

void ThreadFunc2(::shanzhai_tf::Notifier *notifier) {
  std::this_thread::sleep_for(std::chrono::seconds(5));
  predicate = true;
  std::cout << "ThreadFunc2-1\n";
  notifier->Notify(true);
  std::this_thread::sleep_for(std::chrono::seconds(5));
  std::cout << "ThreadFunc2-2\n";
}

int main() {
  {
    ::shanzhai_tf::Notifier notifier(2);

    std::thread t1(ThreadFunc1, &notifier);
    std::thread t2(ThreadFunc2, &notifier);

    t1.join();
    t2.join();
  }

  return 0;
}
