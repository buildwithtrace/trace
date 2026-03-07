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

#include <amplitude_client.h>
#include <kicad_curl/kicad_curl.h>
#include <kicad_curl/kicad_curl_easy.h>
#include <wx/log.h>
#include <wx/config.h>

#include <chrono>
#include <thread>
#include <random>
#include <sstream>

AMPLITUDE_CLIENT* AMPLITUDE_CLIENT::s_instance = nullptr;


AMPLITUDE_CLIENT& AMPLITUDE_CLIENT::Instance()
{
    if( !s_instance )
        s_instance = new AMPLITUDE_CLIENT();

    return *s_instance;
}


void AMPLITUDE_CLIENT::Destroy()
{
    if( s_instance )
    {
        s_instance->Shutdown();
        delete s_instance;
        s_instance = nullptr;
    }
}


void AMPLITUDE_CLIENT::Init( const std::string& aApiKey )
{
    m_apiKey = aApiKey;

    if( m_apiKey.empty() )
    {
        wxLogDebug( "Amplitude analytics disabled (no API key)" );
        return;
    }

    wxConfigBase* cfg = wxConfigBase::Get();

    if( cfg )
    {
        wxString storedId;

        if( cfg->Read( "/amplitude/device_id", &storedId ) && !storedId.IsEmpty() )
        {
            m_deviceId = storedId.ToStdString();
        }
        else
        {
            std::random_device rd;
            std::mt19937 gen( rd() );
            std::uniform_int_distribution<uint64_t> dist;
            std::ostringstream oss;
            oss << std::hex << dist( gen ) << dist( gen );
            m_deviceId = oss.str();
            cfg->Write( "/amplitude/device_id", wxString::FromUTF8( m_deviceId ) );
            cfg->Flush();
        }
    }

    m_running = true;
    m_worker = std::thread( &AMPLITUDE_CLIENT::WorkerLoop, this );

    wxLogDebug( "Amplitude analytics initialized" );
}


void AMPLITUDE_CLIENT::SetUserId( const std::string& aUserId )
{
    std::lock_guard<std::mutex> lock( m_mutex );
    m_userId = aUserId;
}


void AMPLITUDE_CLIENT::SetDeviceId( const std::string& aDeviceId )
{
    std::lock_guard<std::mutex> lock( m_mutex );
    m_deviceId = aDeviceId;
}


void AMPLITUDE_CLIENT::SetUserProperties( const nlohmann::json& aProps )
{
    std::lock_guard<std::mutex> lock( m_mutex );
    m_userProperties = aProps;
}


void AMPLITUDE_CLIENT::Track( const std::string& aEventType,
                              const nlohmann::json& aProperties )
{
    if( m_apiKey.empty() )
        return;

    nlohmann::json event;
    event["event_type"] = aEventType;
    event["event_properties"] = aProperties;

    auto now = std::chrono::system_clock::now();
    event["time"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch() ).count();

    {
        std::lock_guard<std::mutex> lock( m_mutex );

        if( !m_userId.empty() )
            event["user_id"] = m_userId;

        if( !m_deviceId.empty() )
            event["device_id"] = m_deviceId;

        if( !m_userProperties.empty() )
            event["user_properties"] = m_userProperties;

        m_queue.push_back( std::move( event ) );
    }

    m_cv.notify_one();
}


void AMPLITUDE_CLIENT::WorkerLoop()
{
    while( m_running )
    {
        std::vector<nlohmann::json> batch;

        {
            std::unique_lock<std::mutex> lock( m_mutex );

            m_cv.wait_for( lock, std::chrono::seconds( FLUSH_INTERVAL_S ),
                           [this]{ return m_queue.size() >= FLUSH_THRESHOLD
                                          || !m_running; } );

            if( !m_queue.empty() )
                batch.swap( m_queue );
        }

        if( !batch.empty() )
            SendBatch( batch );
    }

    // Final drain on shutdown
    std::vector<nlohmann::json> remaining;
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        remaining.swap( m_queue );
    }

    if( !remaining.empty() )
        SendBatch( remaining );
}


void AMPLITUDE_CLIENT::Flush()
{
    m_cv.notify_one();
}


void AMPLITUDE_CLIENT::Shutdown()
{
    if( m_running )
    {
        m_running = false;
        m_cv.notify_one();

        if( m_worker.joinable() )
            m_worker.join();
    }

    Flush();
    m_apiKey.clear();
}


void AMPLITUDE_CLIENT::SendBatch( const std::vector<nlohmann::json>& aEvents )
{
    if( aEvents.empty() || m_apiKey.empty() )
        return;

    static constexpr int MAX_RETRIES = 2;
    int retryCount = 0;

    while( retryCount <= MAX_RETRIES )
    {
        try
        {
            nlohmann::json payload;
            payload["api_key"] = m_apiKey;
            payload["events"] = aEvents;

            std::string body = payload.dump();

            KICAD_CURL_EASY curl;
            curl.SetURL( AMPLITUDE_ENDPOINT );
            curl.SetPostFields( body );
            curl.SetHeader( "Content-Type", "application/json" );
            curl.SetHeader( "Accept", "*/*" );
            curl.SetFollowRedirects( true );
            curl.SetConnectTimeout( 5 );
            curl_easy_setopt( curl.GetCurl(), CURLOPT_TIMEOUT, 10L );

            curl.Perform();

            int httpCode = curl.GetResponseStatusCode();
            if( httpCode == 200 )
            {
                wxLogDebug( "Amplitude: sent %zu events (HTTP 200)",
                            aEvents.size() );
                return;
            }
            else if( httpCode == 0 && retryCount < MAX_RETRIES )
            {
                retryCount++;
                std::this_thread::sleep_for( std::chrono::milliseconds( 500 * retryCount ) );
                continue;
            }
            else
            {
                wxLogDebug( "Amplitude: batch failed with HTTP %d (%zu events, retries=%d)",
                            httpCode, aEvents.size(), retryCount );
                return;
            }
        }
        catch( const std::exception& e )
        {
            if( retryCount < MAX_RETRIES )
            {
                retryCount++;
                std::this_thread::sleep_for( std::chrono::milliseconds( 500 * retryCount ) );
                continue;
            }
            wxLogDebug( "Amplitude send error after %d retries: %s", retryCount, e.what() );
            return;
        }
    }
}
