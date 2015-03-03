#pragma once

#include "macros.hpp"

#include "../std/thread.hpp"

/// This class remembers id of a thread on which it was created, and
/// then can be used to verify that CalledOnOriginalThread() is called
/// from a thread on which it was created.
class ThreadChecker
{
public:
  ThreadChecker();

  /// \return True if this method is called from a thread on which
  ///         current ThreadChecker's instance was created, false otherwise.
  bool CalledOnOriginalThread() const;

private:
  thread::id const m_id;

  DISALLOW_COPY_AND_MOVE(ThreadChecker);
};
