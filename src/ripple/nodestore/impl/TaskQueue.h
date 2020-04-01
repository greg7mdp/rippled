//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2019 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_NODESTORE_TASKQUEUE_H_INCLUDED
#define RIPPLE_NODESTORE_TASKQUEUE_H_INCLUDED

#include <ripple/core/Stoppable.h>
#include <ripple/core/impl/Workers.h>

#include <functional>
#include <queue>

namespace ripple {
namespace NodeStore {

class TaskQueue : public Stoppable, private Workers::Callback
{
public:
    explicit TaskQueue(Stoppable& parent);

    void
    onStop() override;

    /** Adds a task to the queue

        @param task std::function with signature void()
    */
    void
    addTask(std::function<void()> task);

private:
    std::mutex mutex_;
    Workers workers_;
    std::queue<std::function<void()>> tasks_;

    void
    processTask(int instance) override;
};

}  // namespace NodeStore
}  // namespace ripple

#endif