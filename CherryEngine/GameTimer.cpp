#include "GameTimer.h"
#include <windows.h>
GameTimer::GameTimer() :
    mSecondsPerCount(0.0), mDeltaTime(-1.0), mBaseTime(0),
    mPausedTime(0), mPrevTime(0), mCurrentTime(0), mStopped(false)
{
    __int64 countsPerSec;
    QueryPerformanceFrequency((LARGE_INTEGER*)&countsPerSec);
    mSecondsPerCount = 1.0 / (double)countsPerSec;
}

float GameTimer::GameTime() const
{
    return (float)mDeltaTime;
}

float GameTimer::DeltaTime() const
{
    return (float)mDeltaTime;
}

float GameTimer::TotalTime() const
{
    if (mStopped)
    {
        return (float)(((mStopTime - mPausedTime) - mBaseTime) * mSecondsPerCount);
    }

    else
    {
        return (float)(((mCurrentTime - mPausedTime) - mBaseTime) * mSecondsPerCount);
    }
}

void GameTimer::Reset()
{
    __int64 currTime;
    QueryPerformanceCounter((LARGE_INTEGER*)&currTime);
    mBaseTime = currTime;
    mPrevTime = currTime;
    mStopTime = 0;
    mStopped = false;
}

void GameTimer::Start()
{
    __int64 startTime;
    QueryPerformanceCounter((LARGE_INTEGER*)&startTime);

    if (mStopped)
    {
        mPausedTime += (startTime - mStopTime);

        mPrevTime = startTime;
        mStopTime = 0;
        mStopped = false;
    }
}

void GameTimer::Stop()
{
    if (!mStopped)
    {
        __int64 currTime;
        QueryPerformanceCounter((LARGE_INTEGER*)&currTime);

        mStopTime = currTime;
        mStopped = true;
    }
}

void GameTimer::Tick()
{
    if (mStopped)
    {
        mDeltaTime = 0.0;
        return;
    }

    __int64 currTime;
    QueryPerformanceCounter((LARGE_INTEGER*)&currTime);
    mCurrentTime = currTime;
    mDeltaTime = (mCurrentTime - mPrevTime) * mSecondsPerCount;
    mPrevTime = mCurrentTime;
    if (mDeltaTime < 0.0)
    {
        mDeltaTime = 0.0f;
    }
}
