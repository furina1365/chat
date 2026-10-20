#include "Backend.hpp"

EventLoop::EventLoop() : impl_(std::make_unique<Impl>()) {}
EventLoop::~EventLoop() = default;
bool EventLoop::Init() { return impl_->Init(); }
void EventLoop::Loop() { impl_->Loop(); }
void EventLoop::Quit() { impl_->Quit(); }
void EventLoop::Shutdown() { impl_->Shutdown(); }
bool EventLoop::IsInLoopThread() const { return impl_->IsInLoopThread(); }
bool EventLoop::RunInLoop(Functor callback) { return impl_->RunInLoop(std::move(callback)); }
bool EventLoop::QueueInLoop(Functor callback) { return impl_->QueueInLoop(std::move(callback)); }
TimerQueue &EventLoop::timerQueue() { return impl_->timerQueue(); }
void EventLoop::SetEveryLoopCallback(Functor callback) { impl_->SetEveryLoopCallback(std::move(callback)); }
void EventLoop::SetShutdownCallback(Functor callback) { impl_->SetShutdownCallback(std::move(callback)); }
