#pragma once

// 持有 fd、线程等唯一资源的类继承它，就不用在每个类里重复删除拷贝
// 构造和拷贝赋值。构造/析构设为 protected，只允许作为基类使用。
class NonCopyable
{
protected:
    NonCopyable() = default;
    ~NonCopyable() = default;

private:
    NonCopyable(const NonCopyable &) = delete;
    NonCopyable &operator=(const NonCopyable &) = delete;
};

