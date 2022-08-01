#include "policy.h"
#include <map>
#include <queue>
#include <iostream>
using namespace std;

map<int, Event::Task> CPU_Queue;  //int类型存放的是deadline
map<int, Event::Task> IO_Queue;

int cur_time = -1;   //现在的时间
Action policy(const std::vector<Event>& events, int current_cpu,
              int current_io) {
  Action next;

  int earlist = 1000000;
  int length = events.size();
  for(int i = 0; i < length; i++){
    if(events[i].type == Event::Type::kTimer){ //时钟中断
      cur_time = events[i].time;  //更新时间
    }else if(events[i].type == Event::Type::kTaskArrival){   //新任务到达
      CPU_Queue.insert(map<int, Event::Task>::value_type(events[i].task.deadline, events[i].task));
      //添加该任务到列表
    }else if(events[i].type == Event::Type::kTaskFinish){ //任务完成
      for(map<int,Event::Task>::iterator iter = CPU_Queue.begin(); iter != CPU_Queue.end(); iter++){
        if(iter->second.taskId == events[i].task.taskId){  //从列表里删除该任务
          CPU_Queue.erase(iter);
          break;
        }
      }
    }else if(events[i].type == Event::Type::kIoRequest){  //任务请求IO，添加到IO列表中
      IO_Queue.insert(map<int,Event::Task>::value_type(events[i].task.deadline, events[i].task));
      for(map<int,Event::Task>::iterator iter = CPU_Queue.begin(); iter != CPU_Queue.end(); iter++){
        if(iter->second.taskId == events[i].task.taskId){
          CPU_Queue.erase(iter);  //一个任务不能同时占用CPU和IO，所以暂时从CPU队列中移除
          break;
        }
      }

    }else if(events[i].type == Event::Type::kIoEnd){  //任务结束IO
      CPU_Queue.insert(map<int,Event::Task>::value_type(events[i].task.deadline, events[i].task)); //重新添加回CPU队列中
      for(map<int,Event::Task>::iterator iter = IO_Queue.begin(); iter != IO_Queue.end(); iter++){
        if(iter->second.taskId == events[i].task.taskId){
          IO_Queue.erase(iter);
          break;
        }
      }
    }
  }

  next.cpuTask = current_cpu;
  next.ioTask = current_io;

  if(current_io == 0){   //只有当IO资源空闲时才能执行新的IO任务
    if(!IO_Queue.empty()){
      map<int,Event::Task>::iterator iter;
      for(iter = IO_Queue.begin(); iter != IO_Queue.end(); iter++){
        if(iter->first > cur_time){  //找到截止时间最近且未超时的任务执行
          break;
        }
      }
      if(iter == IO_Queue.end()){  //若全超时，从头开始执行
        iter = IO_Queue.begin();
      }
      next.ioTask = iter->second.taskId;
    }
  }

  map<int,Event::Task>::iterator iter;
  for(iter = CPU_Queue.begin(); iter != CPU_Queue.end(); iter++){
    if(iter->first > cur_time){  //找到截止时间最近且未超时的任务执行
      break;
    }
  }
  if(iter == CPU_Queue.end()){
    iter = CPU_Queue.begin();
  }
  next.cpuTask = iter->second.taskId;

  return next;
}