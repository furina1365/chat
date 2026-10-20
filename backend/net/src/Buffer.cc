#include "net/Buffer.hpp"
#include <algorithm>
#include <cassert>

Buffer::Buffer(size_t maxBytes) : maxBytes_(maxBytes)
{
    }

size_t Buffer::readableBytes() const
{ return writeIndex_ - readIndex_; }

size_t Buffer::writableBytes() const
{ return buf_.size() - writeIndex_; }

size_t Buffer::maxBytes() const
{ return maxBytes_; }

bool Buffer::overLimit() const
{ return readableBytes() >= maxBytes_; }

const char *Buffer::peek() const
{
        return readableBytes() > 0 ? &buf_[readIndex_] : nullptr;
    }

bool Buffer::append(const char *data, size_t len)
{
        if (len == 0)
        {
            return true;
        }
        if (len > maxBytes_ - readableBytes())
        {
            return false;
        }
        if (writableBytes() < len)
        {
            makeSpace(len);
        }
        std::copy(data, data + len, &buf_[writeIndex_]);
        writeIndex_ += len;
        return true;
    }

bool Buffer::append(const std::string &s)
{ return append(s.data(), s.size()); }

char *Buffer::prepareWrite(size_t len)
{
        if (len > maxBytes_ - readableBytes()) return nullptr;
        if (writableBytes() < len) makeSpace(len);
        return buf_.empty() ? nullptr : buf_.data() + writeIndex_;
    }

void Buffer::commitWrite(size_t len)
{
        assert(len <= writableBytes() && len <= maxBytes_ - readableBytes());
        writeIndex_ += len;
    }

void Buffer::retrieve(size_t len)
{
        readIndex_ += len;
        if (readIndex_ == writeIndex_)
        {
            readIndex_ = writeIndex_ = 0;
        }
    }

void Buffer::retrieveAll()
{ readIndex_ = writeIndex_ = 0; }

void Buffer::makeSpace(size_t len)
{
        if (buf_.empty())
        {
            buf_.resize(len < kInitialSize ? kInitialSize : len);
            return;
        }
        if (readIndex_ + writableBytes() >= len)
        {
            size_t readable = readableBytes();
            std::copy(buf_.begin() + readIndex_, buf_.begin() + writeIndex_, buf_.begin());
            readIndex_ = 0;
            writeIndex_ = readable;
        }
        else
        {
            buf_.resize(writeIndex_ + len);
        }
    }
