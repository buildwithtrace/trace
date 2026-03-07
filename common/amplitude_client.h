/*
 * This program source code file is part of Trace, an AI-native PCB design application.
 *
 * Copyright The Trace Developers, see TRACE_AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 */

#ifndef AMPLITUDE_CLIENT_H
#define AMPLITUDE_CLIENT_H

#include <kicommon.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <nlohmann/json.hpp>

/**
 * High-throughput Amplitude analytics client with async delivery.
 *
 * Events are queued from any thread and delivered in the background.
 * A dedicated worker thread flushes when the batch threshold is reached
 * or after a periodic interval, whichever comes first.
 *
 * Memory: each queued event is ~200-500 bytes of JSON.  Even 10k events
 * would be ~5 MB, well within reason.  The batch threshold keeps the
 * queue drained continuously.
 */
class KICOMMON_API AMPLITUDE_CLIENT
{
public:
    static AMPLITUDE_CLIENT& Instance();
    static void Destroy();

    void Init( const std::string& aApiKey );
    void SetUserId( const std::string& aUserId );
    void SetDeviceId( const std::string& aDeviceId );
    void SetUserProperties( const nlohmann::json& aProps );

    /**
     * Queue an event for delivery.  Non-blocking, thread-safe.
     */
    void Track( const std::string& aEventType,
                const nlohmann::json& aProperties = nlohmann::json::object() );

    /** Wake the worker to send queued events now.  Non-blocking. */
    void Flush();

    /** Flush and stop the worker thread.  Called once on app exit. */
    void Shutdown();

private:
    AMPLITUDE_CLIENT() = default;
    ~AMPLITUDE_CLIENT() = default;

    AMPLITUDE_CLIENT( const AMPLITUDE_CLIENT& ) = delete;
    AMPLITUDE_CLIENT& operator=( const AMPLITUDE_CLIENT& ) = delete;

    void WorkerLoop();
    void SendBatch( const std::vector<nlohmann::json>& aEvents );

    static AMPLITUDE_CLIENT* s_instance;

    std::string    m_apiKey;
    std::string    m_userId;
    std::string    m_deviceId;
    nlohmann::json m_userProperties = nlohmann::json::object();

    std::mutex                  m_mutex;
    std::condition_variable     m_cv;
    std::vector<nlohmann::json> m_queue;

    std::thread     m_worker;
    std::atomic<bool> m_running{ false };

    static constexpr size_t FLUSH_THRESHOLD  = 5;
    static constexpr int    FLUSH_INTERVAL_S = 30;
    static constexpr const char* AMPLITUDE_ENDPOINT = "https://api2.amplitude.com/2/httpapi";
};

#endif // AMPLITUDE_CLIENT_H
