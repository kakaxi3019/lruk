/*
基于以下链接实现的LRU-K算法：
https://github.com/lidaohang/ceph_study/blob/master/LRU-K%E5%92%8C2Q%E7%BC%93%E5%AD%98%E7%AE%97%E6%B3%95%E4%BB%8B%E7%BB%8D.md

1.数据第一次被访问，加入到访问历史列表；

2.如果数据在访问历史列表里后没有达到K次访问，则按照一定规则（FIFO，LRU）淘汰；

3.当访问历史队列中的数据访问次数达到K次后，将数据索引从历史队列删除，将数据移到缓存队列中，并缓存此数据，缓存队列重新按照时间排序；

4.缓存数据队列中被再次访问后，重新排序；

5.需要淘汰数据时，淘汰缓存队列中排在末尾的数据，即：淘汰“倒数第K次访问离现在最久”的数据。
*/

#include <iostream>
#include <unordered_map>
#include <queue>
#include <list>
#include <chrono> //用于steady_clock::time_point
#include <assert.h>

using namespace std;
using namespace std::chrono; //用于steady_clock::time_point



// 通用模板，非字符串类型的情况下，将值转换为字符串并返回
template <typename T>
std::string to_string_if_not_string(const T& value, typename std::enable_if<!std::is_same<T, std::string>::value>::type* = 0) {
    return std::to_string(value);  // 使用 std::to_string 将非字符串类型转换为字符串
}

// 针对字符串类型的模板特化，直接返回字符串本身
template <typename T>
std::string to_string_if_not_string(const T& value, typename std::enable_if<std::is_same<T, std::string>::value>::type* = 0) {
    return value;  // 字符串类型，直接返回
}

template <typename KEY, typename VALUE>
class CacheEntry
{
public:
    KEY key_;
    VALUE value_;
    queue<steady_clock::time_point> access_time_; // 最近K次访问时间记录,时间早的优先被淘汰

    CacheEntry(KEY k, VALUE v) : key_(k), value_(v) {}
};

//cachelist排序比较函数，时间从新到旧的顺序排序
template <typename KEY, typename VALUE>
bool compareByAccessTime(const CacheEntry<KEY, VALUE>& a, const CacheEntry<KEY, VALUE>& b) {
    return a.access_time_.front() > b.access_time_.front();  // 最新的时间排在前面
}

template <typename KEY, typename VALUE>
class LRUK_Cache
{
    int capacity_;                                                           // 最大可缓存上限
    int k_;                                                                  // 超过K次之后可移入cacheList
    list<CacheEntry<KEY, VALUE> > historyList_;                              // 保存历史记录，超过K次访问后移入cacheList。
    list<CacheEntry<KEY, VALUE> > cacheList_;                                // 保存热数据记录，查找时优先查找。
    unordered_map<KEY, typename list<CacheEntry<KEY, VALUE> >::iterator> history_map_; // 用于快速定位historyList_
    unordered_map<KEY, typename list<CacheEntry<KEY, VALUE> >::iterator> cache_map_;   // 用于快速定位cacheList_

    typename list<CacheEntry<KEY, VALUE> >::iterator findVictimFromCache()
    {
        // cachelist中已经是按时间从新到旧的顺序排序了，因此list中最后一元素就是需要被淘汰的数据
        return --cacheList_.end();
    }

    typename list<CacheEntry<KEY, VALUE> >::iterator findVictimFromHistory()
    {
        // historylist按先进先出的原则淘汰数据,最早的数据在尾部
        return --historyList_.end();
    }
    
    // 根据k查找cachelist中的元素
    typename list<CacheEntry<KEY, VALUE> >::iterator findCacheEntry(const KEY &k)
    {
        typename list<CacheEntry<KEY, VALUE> >::iterator ret = cacheList_.begin();
        for (; ret != cacheList_.end(); ret++)
        {
            if (ret->key_ == k)
                break;
        }
        return ret;
    }

    //从cachelist中查找元素，如果找到，需要更新时间，然后根据时间进行重排序
    typename list<CacheEntry<KEY, VALUE> >::iterator getFromCacheList(const KEY &k)
    {
        typename list<CacheEntry<KEY, VALUE> >::iterator ret = cacheList_.end();
        typename unordered_map<KEY, typename list<CacheEntry<KEY, VALUE> >::iterator>::iterator it = cache_map_.find(k);
        if (it != cache_map_.end())
        {
            //找到key对应的数据
            it->second->access_time_.push(steady_clock::now());
            //只记录前K次时间
            if (it->second->access_time_.size() > k_)
            {
                it->second->access_time_.pop();
                //这里由于将最老的访问时间移除了，因此需要重新排序
                cacheList_.sort(compareByAccessTime<KEY,VALUE>);
                ret = findCacheEntry(k);
                cache_map_.erase(k);
                cache_map_.emplace(k,ret);
            }
        }
        return ret;
    }

    // 从历史数据中查找元素
    // 如果找到，更新时间，然后：
    //    当热度大于等于k时，如果cachelist没有满，则插入cachelist，重新排序。
    //        如果满了则先从cachelist中找到最老的数据，移回到historylist头部，再将查找的数据移入cachelist中，重新排序。
    //    当热度小于k时：
    //        将元素移到historylist头部。
    typename list<CacheEntry<KEY, VALUE> >::iterator getFromHistoryList(const KEY &k)
    {
        typename list<CacheEntry<KEY, VALUE> >::iterator ret = historyList_.end();
        typename unordered_map<KEY, typename list<CacheEntry<KEY, VALUE> >::iterator>::iterator it = history_map_.find(k);
        if (it != history_map_.end())
        {
            //找到key对应的数据
            typename list<CacheEntry<KEY, VALUE> >::iterator entry_it = it->second;
            ret = entry_it;
            entry_it->access_time_.push(steady_clock::now());
            // 超过K次访问，变为热数据
            if (entry_it->access_time_.size() >= k_)
            {
                if (entry_it->access_time_.size() > k_)
                    entry_it->access_time_.pop();
                // cacheList_没有满，可以直接插入
                if (cacheList_.size() < capacity_)
                {
                    cacheList_.emplace(cacheList_.end(),*entry_it);
                    //cache_map_.emplace(k,cacheList_.begin());
                    //从历史数据中移除
                    history_map_.erase(entry_it->key_);
                    historyList_.erase(entry_it);
                    //重新排序
                    cacheList_.sort(compareByAccessTime<KEY,VALUE>);
                    ret = findCacheEntry(k);
                    cache_map_.emplace(k,ret);
                }
                else
                {
                    // cacheList_满了，需要淘汰一个到historyList_
                    typename list<CacheEntry<KEY, VALUE> >::iterator vict = findVictimFromCache();
                    cacheList_.emplace(cacheList_.end(),*entry_it);
                    //cache_map_.emplace((*entry_it).key_,cacheList_.begin());
                    //从cacheList_淘汰的回到历史数据头部
                    historyList_.emplace_front(*vict);
                    history_map_.emplace((*vict).key_,historyList_.begin());
                    //删除
                    cache_map_.erase(vict->key_);
                    cacheList_.erase(vict);
                    history_map_.erase(entry_it->key_);
                    historyList_.erase(entry_it);
                    //重新排序
                    cacheList_.sort(compareByAccessTime<KEY,VALUE>);
                    ret = findCacheEntry(k);
                    cache_map_.emplace(k,ret);
                }
            }
            // 没有超过K次，保留在历史数据中
            else
            {
                // 将刚查询的entry移动到头部
                historyList_.splice(historyList_.begin(), historyList_, entry_it);
            }
        }

        return ret;
    }

public:
    LRUK_Cache(int c, int k) : capacity_(c), k_(k) {}

    VALUE get(KEY k, bool &found)
    {
        // 先从cache中查找
        typename list<CacheEntry<KEY, VALUE> >::iterator entry_it = getFromCacheList(k);
        if (entry_it != cacheList_.end())
        {
            //找到
            found = true;
            return entry_it->value_;
        }

        // 从历史数据中查找
        entry_it = getFromHistoryList(k);
        if (entry_it != historyList_.end())
        {
            //找到
            found = true;
            return entry_it->value_;
        }
        //未从任何缓存中找到
        found = false;
        return VALUE();

    }

    void put(KEY k, VALUE v)
    {
        // 先找cache数据
        typename list<CacheEntry<KEY, VALUE> >::iterator entry_it = getFromCacheList(k);
        if (entry_it != cacheList_.end())
        {
            entry_it->value_ = v;
            return;
        }

        // 从历史数据中查找
        entry_it = getFromHistoryList(k);
        if (entry_it != historyList_.end())
        {
            entry_it->value_ = v;
            return;
        }

        //没有找到相同key的记录，作为新记录插入
        //如果历史数据没有满，则直接插入
        if (historyList_.size() < capacity_)
        {
            historyList_.emplace_front(k,v);
            historyList_.begin()->access_time_.push(steady_clock::now());
            history_map_.emplace(k,historyList_.begin());
            return;
        }

        //如果历史数据满了，则淘汰一个最老的记录
        typename list<CacheEntry<KEY, VALUE> >::iterator vict = findVictimFromHistory();
        history_map_.erase(vict->key_);
        historyList_.erase(vict);
        //插入新记录
        historyList_.emplace_front(k,v);
        historyList_.begin()->access_time_.push(steady_clock::now());
        history_map_.emplace(k,historyList_.begin());

        return;
    }

    void clear()
    {
        history_map_.clear();
        historyList_.clear();
        cache_map_.clear();
        cacheList_.clear();
    }

    void print()
    {
        string msg;
        if (cacheList_.size() > 0)
        {
            msg += "cacheList:\n";
            int num = 0;
            typename list<CacheEntry<KEY, VALUE> >::iterator it = cacheList_.begin();
            for (; it != cacheList_.end(); it++)
            {
                msg += "[";
                msg += to_string(num);
                msg += "] key=";
                msg += to_string_if_not_string(it->key_);
                msg += ", ";
                msg += "value=";
                msg += to_string_if_not_string(it->value_);
                msg += "\n";
                num++;
            }
        }
        else
            msg += "cacheList is empty.\n";

        if (historyList_.size() > 0)
        {
            msg += "historyList:\n";
            int num = 0;
            typename list<CacheEntry<KEY, VALUE> >::iterator it = historyList_.begin();
            for (; it != historyList_.end(); it++)
            {
                msg += "[";
                msg += to_string(num);
                msg += "] key=";
                msg += to_string_if_not_string(it->key_);
                msg += ", ";
                msg += "value=";
                msg += to_string_if_not_string(it->value_);
                msg += "\n";
                num++;
            }
        }
        else
            msg += "historyList is empty.\n";

        cout<<msg;
    }
};

int main()
{
    LRUK_Cache<int,string> cache(3,2);
    bool found = false;
    string val;
    //数据第一次访问，加入历史访问列表中
    cache.put(1,"A");
    cache.put(2,"B");
    cache.put(3,"C");
    //从历史访问列表中淘汰最早的数据key=1
    cache.put(4,"D");
    //key=2数据移入缓存列表，并从历史访问列表中移除
    val = cache.get(2,found);
    assert(found && val == "B");
    //key=3数据移入缓存列表，通过重新排序后处于头部位置，并从历史访问列表中移除
    cache.put(3,"C1");
    //key=4数据移入缓存列表，通过重新排序后处于头部位置，并从历史访问列表中移除
    val = cache.get(4,found);
    assert(found && val == "D");
    //填充满历史访问列表
    cache.put(5,"E");
    cache.put(6,"F");
    cache.put(7,"G");
    //key=5数据移入缓存列表，通过重新排序后处于头部位置，从缓存列表淘汰key=2的数据，放入历史列表头部
    cache.put(5,"E1");
    //key=6数据移入缓存列表，通过重新排序后处于头部位置，从缓存列表淘汰key=3的数据，放入历史列表头部
    cache.put(6,"F1");
    //key=7数据移入缓存列表，通过重新排序后处于头部位置，从缓存列表淘汰key=4的数据，放入历史列表头部
    val = cache.get(7,found);
    assert(found && val == "G");
    //key=5数据更新后处于缓存列表头部
    cache.put(5,"E2");
    //缓存中保存的值为：
    //cachelist: [0]key=5,value="E2" [1]key=7,value="G" [2]key=6 value="F1"
    //historylist: [0]key=4 value="D" [1]key=3 value="C1" [2]key=2 value="B"
    cache.print();
    cache.clear();
    return 0;
}