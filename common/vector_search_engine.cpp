#include "vector_search_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>

#include <wx/filename.h>
#include <wx/log.h>
#include <wx/dir.h>

#ifndef TRACE_NO_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

#include <wordpiece_tokenizer.hpp>
#include <nlohmann/json.hpp>

// usearch requires these before inclusion
#if defined( _MSC_VER )
#pragma warning( push )
#pragma warning( disable : 4267 4244 4996 )
#endif

#if defined( __clang__ )
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif

#define USEARCH_USE_OPENMP 0
#define USEARCH_USE_SIMSIMD 0
#include <index_dense.hpp>

#if defined( __clang__ )
#pragma clang diagnostic pop
#endif

#if defined( _MSC_VER )
#pragma warning( pop )
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

static constexpr size_t EMBEDDING_DIM = 384;
static constexpr int    MAX_TOKEN_LEN = 128;


// -- PIMPL structs ---------------------------------------------------------

#ifndef TRACE_NO_ONNXRUNTIME
struct VECTOR_SEARCH_ENGINE::OrtState
{
    Ort::Env             env;
    Ort::Session         session{ nullptr };
    Ort::SessionOptions  sessionOpts;
    Ort::MemoryInfo      memoryInfo{ nullptr };

    OrtState() :
        env( ORT_LOGGING_LEVEL_WARNING, "VectorSearch" ),
        memoryInfo( Ort::MemoryInfo::CreateCpu( OrtArenaAllocator, OrtMemTypeDefault ) )
    {
        sessionOpts.SetIntraOpNumThreads( std::min( (int) std::thread::hardware_concurrency(), 4 ) );
        sessionOpts.SetInterOpNumThreads( 1 );
        sessionOpts.SetGraphOptimizationLevel( GraphOptimizationLevel::ORT_ENABLE_ALL );
    }
};
#else
struct VECTOR_SEARCH_ENGINE::OrtState {};
#endif


struct VECTOR_SEARCH_ENGINE::IndexData
{
    unum::usearch::index_dense_t index;
    bool                         loaded = false;
};


// -- Singleton --------------------------------------------------------------

VECTOR_SEARCH_ENGINE& VECTOR_SEARCH_ENGINE::GetInstance()
{
    static VECTOR_SEARCH_ENGINE s_instance;
    return s_instance;
}


VECTOR_SEARCH_ENGINE::VECTOR_SEARCH_ENGINE() = default;
VECTOR_SEARCH_ENGINE::~VECTOR_SEARCH_ENGINE() = default;


// -- Init -------------------------------------------------------------------

bool VECTOR_SEARCH_ENGINE::Init( const wxString& aModelPath,
                                 const wxString& aTokenizerPath,
                                 const wxString& aIndexDir,
                                 const wxString& aSymbolLibDirs,
                                 const wxString& aFootprintLibDirs )
{
    std::lock_guard<std::mutex> lock( m_initMutex );

    if( m_initialized.load() )
        return true;

    auto t0 = std::chrono::steady_clock::now();

    // 1. Load ONNX Runtime session
#ifndef TRACE_NO_ONNXRUNTIME
    try
    {
        m_ort = std::make_unique<OrtState>();

#ifdef __WXMSW__
        std::wstring modelPathW = aModelPath.ToStdWstring();
        m_ort->session = Ort::Session( m_ort->env, modelPathW.c_str(), m_ort->sessionOpts );
#else
        std::string modelPathStr = aModelPath.ToStdString();
        m_ort->session = Ort::Session( m_ort->env, modelPathStr.c_str(), m_ort->sessionOpts );
#endif

        wxLogDebug( wxT( "AI DEBUG [VectorSearch]: ONNX session loaded from %s" ), aModelPath );
    }
    catch( const Ort::Exception& e )
    {
        wxLogError( wxT( "AI DEBUG [VectorSearch]: Failed to load ONNX model: %s" ),
                    wxString( e.what() ) );
        return false;
    }
#else
    wxLogWarning( wxT( "AI DEBUG [VectorSearch]: ONNX Runtime not available (TRACE_NO_ONNXRUNTIME)" ) );
    return false;
#endif

    // 2. Load tokenizer
    m_tokenizer = std::make_unique<wordpiece::Tokenizer>();

    if( !m_tokenizer->LoadFromJson( aTokenizerPath.ToStdString() ) )
    {
        wxLogError( wxT( "AI DEBUG [VectorSearch]: Failed to load tokenizer from %s" ),
                    aTokenizerPath );
        return false;
    }

    wxLogDebug( wxT( "AI DEBUG [VectorSearch]: Tokenizer loaded from %s" ), aTokenizerPath );

    // 3. Store config
    m_indexDir = aIndexDir.ToStdString();
    m_symbolLibDirs = aSymbolLibDirs.ToStdString();
    m_footprintLibDirs = aFootprintLibDirs.ToStdString();

    // 4. Load or create usearch indices
    m_symbolIndex = std::make_unique<IndexData>();
    m_footprintIndex = std::make_unique<IndexData>();

    auto loadIndex = []( IndexData& idx, const std::string& path,
                         std::unordered_map<uint64_t, std::string>& metaMap,
                         const std::string& metaDbPath ) -> bool
    {
        unum::usearch::metric_punned_t metric( EMBEDDING_DIM,
                                                unum::usearch::metric_kind_t::cos_k,
                                                unum::usearch::scalar_kind_t::f32_k );

        unum::usearch::index_dense_config_t config;
        config.connectivity = 16;
        config.expansion_add = 128;
        config.expansion_search = 64;

        idx.index = unum::usearch::index_dense_t::make( metric, config );

        if( fs::exists( path ) )
        {
            idx.index.load( path.c_str() );
            idx.loaded = true;
            wxLogDebug( wxT( "AI DEBUG [VectorSearch]: Loaded index from %s (%zu vectors)" ),
                        wxString( path ), (size_t) idx.index.size() );
        }
        else
        {
            // Check for raw fallback file from migration script
            std::string rawPath = path + ".raw";
            if( fs::exists( rawPath ) )
            {
                std::ifstream rawFile( rawPath, std::ios::binary );
                if( rawFile.is_open() )
                {
                    uint64_t count;
                    uint32_t dim;
                    rawFile.read( reinterpret_cast<char*>( &count ), sizeof( count ) );
                    rawFile.read( reinterpret_cast<char*>( &dim ), sizeof( dim ) );

                    wxLogDebug( wxT( "AI DEBUG [VectorSearch]: Building index from raw "
                                     "file: %llu vectors, dim=%u" ),
                                (unsigned long long) count, dim );

                    idx.index.reserve( count + 1000 );

                    std::vector<float> vec( dim );
                    for( uint64_t i = 0; i < count; i++ )
                    {
                        uint64_t key;
                        rawFile.read( reinterpret_cast<char*>( &key ), sizeof( key ) );
                        rawFile.read( reinterpret_cast<char*>( vec.data() ),
                                      dim * sizeof( float ) );
                        idx.index.add( key, vec.data() );
                    }

                    idx.index.save( path.c_str() );
                    idx.loaded = true;

                    wxLogDebug( wxT( "AI DEBUG [VectorSearch]: Built & saved HNSW index "
                                     "from raw: %zu vectors" ),
                                (size_t) idx.index.size() );

                    // Remove raw file after successful conversion
                    fs::remove( rawPath );
                }
            }
            else
            {
                idx.index.reserve( 30000 );
                idx.loaded = false;
                wxLogDebug( wxT( "AI DEBUG [VectorSearch]: Created empty index (no file at %s)" ),
                            wxString( path ) );
            }
        }

        // Load metadata from JSON sidecar
        std::string metaSidecar = path + ".meta.json";
        if( fs::exists( metaSidecar ) )
        {
            std::ifstream f( metaSidecar );
            if( f.is_open() )
            {
                try
                {
                    json metaJson = json::parse( f );
                    for( auto& [key, val] : metaJson.items() )
                    {
                        uint64_t k = std::stoull( key );
                        metaMap[k] = val.dump();
                    }
                    wxLogDebug( wxT( "AI DEBUG [VectorSearch]: Loaded %zu metadata entries" ),
                                metaMap.size() );
                }
                catch( const std::exception& e )
                {
                    wxLogWarning( wxT( "AI DEBUG [VectorSearch]: Failed to parse metadata: %s" ),
                                  wxString( e.what() ) );
                }
            }
        }

        return true;
    };

    std::string symbolIdxPath = m_indexDir + "/symbols.usearch";
    std::string footprintIdxPath = m_indexDir + "/footprints.usearch";

    loadIndex( *m_symbolIndex, symbolIdxPath, m_symbolMeta, "" );
    loadIndex( *m_footprintIndex, footprintIdxPath, m_footprintMeta, "" );

    auto elapsed = std::chrono::steady_clock::now() - t0;
    double ms = std::chrono::duration<double, std::milli>( elapsed ).count();
    wxLogDebug( wxT( "AI DEBUG [VectorSearch]: Init completed in %.1fms" ), ms );

    m_initialized.store( true );
    return true;
}


// -- Embedding --------------------------------------------------------------

std::vector<float> VECTOR_SEARCH_ENGINE::embedQueries( const std::vector<std::string>& aQueries )
{
#ifdef TRACE_NO_ONNXRUNTIME
    (void) aQueries;
    return {};
#else
    if( !m_ort || !m_tokenizer )
        return {};

    size_t batchSize = aQueries.size();
    auto   encoded = m_tokenizer->EncodeBatch( aQueries, MAX_TOKEN_LEN );

    // Flatten into contiguous arrays
    std::vector<int64_t> inputIds( batchSize * MAX_TOKEN_LEN );
    std::vector<int64_t> attentionMask( batchSize * MAX_TOKEN_LEN );
    std::vector<int64_t> tokenTypeIds( batchSize * MAX_TOKEN_LEN, 0 );

    for( size_t i = 0; i < batchSize; i++ )
    {
        std::memcpy( &inputIds[i * MAX_TOKEN_LEN], encoded[i].input_ids.data(),
                     MAX_TOKEN_LEN * sizeof( int64_t ) );
        std::memcpy( &attentionMask[i * MAX_TOKEN_LEN], encoded[i].attention_mask.data(),
                     MAX_TOKEN_LEN * sizeof( int64_t ) );
    }

    // Create ORT tensors
    std::array<int64_t, 2> inputShape = { (int64_t) batchSize, MAX_TOKEN_LEN };

    Ort::Value inputIdsTensor = Ort::Value::CreateTensor<int64_t>(
            m_ort->memoryInfo, inputIds.data(), inputIds.size(),
            inputShape.data(), inputShape.size() );

    Ort::Value attentionMaskTensor = Ort::Value::CreateTensor<int64_t>(
            m_ort->memoryInfo, attentionMask.data(), attentionMask.size(),
            inputShape.data(), inputShape.size() );

    Ort::Value tokenTypeIdsTensor = Ort::Value::CreateTensor<int64_t>(
            m_ort->memoryInfo, tokenTypeIds.data(), tokenTypeIds.size(),
            inputShape.data(), inputShape.size() );

    // Model outputs last_hidden_state [batch, seq, 384]; we apply mean pooling
    const char* inputNames[] = { "input_ids", "attention_mask", "token_type_ids" };
    const char* outputNames[] = { "last_hidden_state" };

    Ort::Value inputTensors[] = { std::move( inputIdsTensor ),
                                  std::move( attentionMaskTensor ),
                                  std::move( tokenTypeIdsTensor ) };

    auto results = m_ort->session.Run(
            Ort::RunOptions{ nullptr },
            inputNames, inputTensors, 3,
            outputNames, 1 );

    // last_hidden_state shape: [batch, seq_len, hidden_dim]
    float* outputData = results[0].GetTensorMutableData<float>();
    auto   outputShape = results[0].GetTensorTypeAndShapeInfo().GetShape();

    size_t seqLen = outputShape[1];
    size_t hiddenDim = outputShape[2];

    // Mean pooling: average hidden states weighted by attention mask
    std::vector<float> embeddings( batchSize * hiddenDim, 0.0f );

    for( size_t b = 0; b < batchSize; b++ )
    {
        float maskSum = 0.0f;

        for( size_t s = 0; s < seqLen; s++ )
        {
            float mask = (float) attentionMask[b * MAX_TOKEN_LEN + s];
            maskSum += mask;

            for( size_t d = 0; d < hiddenDim; d++ )
                embeddings[b * hiddenDim + d] += outputData[( b * seqLen + s ) * hiddenDim + d] * mask;
        }

        if( maskSum > 0.0f )
        {
            for( size_t d = 0; d < hiddenDim; d++ )
                embeddings[b * hiddenDim + d] /= maskSum;
        }
    }

    // L2-normalize each embedding
    for( size_t i = 0; i < batchSize; i++ )
        l2Normalize( &embeddings[i * hiddenDim], hiddenDim );

    return embeddings;
#endif // TRACE_NO_ONNXRUNTIME
}


void VECTOR_SEARCH_ENGINE::l2Normalize( float* vec, size_t dim )
{
    float norm = 0.0f;

    for( size_t i = 0; i < dim; i++ )
        norm += vec[i] * vec[i];

    norm = std::sqrt( norm );

    if( norm > 1e-12f )
    {
        float inv = 1.0f / norm;
        for( size_t i = 0; i < dim; i++ )
            vec[i] *= inv;
    }
}


// -- Search -----------------------------------------------------------------

std::string VECTOR_SEARCH_ENGINE::Search( const std::string& aMode,
                                          const std::vector<std::string>& aQueries,
                                          int aTopK )
{
    auto t0 = std::chrono::steady_clock::now();

    if( !m_initialized.load() )
        return R"({"error": "Vector search engine not initialized"})";

    // Select index
    IndexData* idx = nullptr;
    std::unordered_map<uint64_t, std::string>* metaMap = nullptr;

    if( aMode == "symbol" )
    {
        idx = m_symbolIndex.get();
        metaMap = &m_symbolMeta;
    }
    else if( aMode == "footprint" )
    {
        idx = m_footprintIndex.get();
        metaMap = &m_footprintMeta;
    }
    else
    {
        return R"({"error": "Invalid mode, must be 'symbol' or 'footprint'"})";
    }

    if( !idx || idx->index.size() == 0 )
        return R"({"results": {}, "stale_files": []})";

    // Embed queries
    auto tEmbed = std::chrono::steady_clock::now();
    std::vector<float> queryVecs = embedQueries( aQueries );
    double embedMs = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - tEmbed ).count();

    if( queryVecs.empty() )
        return R"({"error": "Failed to embed queries"})";

    // Search index (shared lock for concurrent reads)
    auto tSearch = std::chrono::steady_clock::now();
    json resultsJson = json::object();

    {
        std::shared_lock<std::shared_mutex> lock( m_indexMutex );

        for( size_t qi = 0; qi < aQueries.size(); qi++ )
        {
            const float* qvec = &queryVecs[qi * EMBEDDING_DIM];

            auto searchResult = idx->index.search( qvec, (size_t) aTopK );

            json queryResults = json::array();

            for( size_t ri = 0; ri < searchResult.size(); ri++ )
            {
                auto  match = searchResult[ri];
                float distance = match.distance;
                float score = 1.0f - distance; // cosine distance -> similarity

                auto metaIt = metaMap->find( match.member.key );
                if( metaIt != metaMap->end() )
                {
                    try
                    {
                        json meta = json::parse( metaIt->second );
                        meta["score"] = std::round( score * 10000.0f ) / 10000.0f;
                        queryResults.push_back( meta );
                    }
                    catch( ... )
                    {
                        json fallback;
                        fallback["id"] = std::to_string( match.member.key );
                        fallback["score"] = std::round( score * 10000.0f ) / 10000.0f;
                        queryResults.push_back( fallback );
                    }
                }
            }

            resultsJson[aQueries[qi]] = queryResults;
        }
    }

    double searchMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - tSearch ).count();

    double totalMs = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - t0 ).count();

    wxLogDebug( wxT( "AI DEBUG [VectorSearch]: Search %zu queries: embed=%.1fms "
                     "search=%.1fms total=%.1fms" ),
                aQueries.size(), embedMs, searchMs, totalMs );

    json output;
    output["results"] = resultsJson;
    output["stale_files"] = json::array();

    // Trigger background freshness check after returning results
    std::string libDirStr = ( aMode == "symbol" ) ? m_symbolLibDirs : m_footprintLibDirs;
    if( !libDirStr.empty() )
    {
        std::vector<std::string> libDirs;
        std::istringstream       ss( libDirStr );
        std::string              dir;
        while( std::getline( ss, dir, ':' ) )
        {
            if( !dir.empty() )
                libDirs.push_back( dir );
        }
        CheckFreshnessAndReindex( aMode, libDirs );
    }

    return output.dump();
}


// -- Freshness & Reindex ---------------------------------------------------

void VECTOR_SEARCH_ENGINE::CheckFreshnessAndReindex( const std::string& aMode,
                                                     const std::vector<std::string>& aLibDirs )
{
    if( m_reindexRunning.load() )
        return;

    // Scan library directories for file changes
    std::vector<std::string> staleFiles;
    std::string              idxPath;

    if( aMode == "symbol" )
        idxPath = m_indexDir + "/symbols.usearch";
    else
        idxPath = m_indexDir + "/footprints.usearch";

    // Get index modification time as baseline
    fs::file_time_type idxMtime{};

    if( fs::exists( idxPath ) )
        idxMtime = fs::last_write_time( idxPath );

    for( const auto& dir : aLibDirs )
    {
        if( !fs::exists( dir ) )
            continue;

        std::string ext = ( aMode == "symbol" ) ? ".kicad_sym" : ".kicad_mod";

        for( const auto& entry : fs::recursive_directory_iterator( dir ) )
        {
            if( entry.is_regular_file() && entry.path().extension() == ext )
            {
                if( entry.last_write_time() > idxMtime )
                    staleFiles.push_back( entry.path().string() );
            }
        }
    }

    if( staleFiles.empty() )
    {
        wxLogDebug( wxT( "AI DEBUG [VectorSearch]: Index is up-to-date for mode=%s" ),
                    wxString( aMode ) );
        return;
    }

    wxLogDebug( wxT( "AI DEBUG [VectorSearch]: %zu stale files, starting background reindex" ),
                staleFiles.size() );

    // Spawn background reindex
    m_reindexRunning.store( true );
    std::string mode = aMode;

    std::thread( [this, mode, staleFiles]()
    {
        try
        {
            ReindexFiles( mode, staleFiles );
        }
        catch( const std::exception& e )
        {
            wxLogWarning( wxT( "AI DEBUG [VectorSearch]: Reindex failed: %s" ),
                          wxString( e.what() ) );
        }

        m_reindexRunning.store( false );
    } ).detach();
}


void VECTOR_SEARCH_ENGINE::ReindexFiles( const std::string& aMode,
                                         const std::vector<std::string>& aFiles )
{
    // Select target index
    IndexData* idx;
    std::unordered_map<uint64_t, std::string>* metaMap;
    std::string idxPath;

    if( aMode == "symbol" )
    {
        idx = m_symbolIndex.get();
        metaMap = &m_symbolMeta;
        idxPath = m_indexDir + "/symbols.usearch";
    }
    else
    {
        idx = m_footprintIndex.get();
        metaMap = &m_footprintMeta;
        idxPath = m_indexDir + "/footprints.usearch";
    }

    if( !idx )
        return;

    wxLogDebug( wxT( "AI DEBUG [VectorSearch]: Reindexing %zu %s files" ),
                aFiles.size(), wxString( aMode ) );

    auto t0 = std::chrono::steady_clock::now();

    // For each file, parse components and embed them
    uint64_t nextKey = idx->index.size();
    std::vector<std::string> textsToEmbed;
    std::vector<json>        metadatas;

    for( const auto& filePath : aFiles )
    {
        std::ifstream f( filePath );
        if( !f.is_open() )
            continue;

        std::string content( ( std::istreambuf_iterator<char>( f ) ),
                             std::istreambuf_iterator<char>() );

        fs::path p( filePath );
        std::string libName = p.stem().string();

        if( aMode == "symbol" )
        {
            // Parse .kicad_sym: find (symbol "Name" ...) blocks
            std::istringstream stream( content );
            std::string        line;
            std::regex         symRegex( R"re(^\t\(symbol "([^"]+)")re" );
            std::regex         unitVariantRegex( R"re(.+_\d+_\d+$)re" );

            while( std::getline( stream, line ) )
            {
                std::smatch match;
                if( std::regex_search( line, match, symRegex ) )
                {
                    std::string name = match[1].str();
                    if( std::regex_match( name, unitVariantRegex ) )
                        continue;

                    // Extract description property
                    std::string description = name + " component from " + libName;
                    std::string keywords;
                    // Simple property extraction from nearby lines
                    size_t pos = content.find( "(symbol \"" + name + "\"" );
                    if( pos != std::string::npos )
                    {
                        size_t descPos = content.find( "(property \"Description\" \"", pos );
                        if( descPos != std::string::npos && descPos - pos < 5000 )
                        {
                            size_t valStart = descPos + 25;
                            size_t valEnd = content.find( "\"", valStart );
                            if( valEnd != std::string::npos )
                                description = content.substr( valStart, valEnd - valStart );
                        }

                        size_t kwPos = content.find( "(property \"ki_keywords\" \"", pos );
                        if( kwPos != std::string::npos && kwPos - pos < 5000 )
                        {
                            size_t valStart = kwPos + 24;
                            size_t valEnd = content.find( "\"", valStart );
                            if( valEnd != std::string::npos )
                                keywords = content.substr( valStart, valEnd - valStart );
                        }
                    }

                    std::string embedText = name + " " + libName + " " + description + " " + keywords;
                    textsToEmbed.push_back( embedText );

                    json meta;
                    meta["name"] = name;
                    meta["library"] = libName;
                    meta["description"] = description;
                    meta["keywords"] = keywords;
                    metadatas.push_back( meta );
                }
            }
        }
        else
        {
            // Parse .kicad_mod: single footprint per file
            std::string name = p.stem().string();
            std::string library = p.parent_path().stem().string();
            library = library.substr( 0, library.find( ".pretty" ) );

            std::string description = name + " footprint from " + library;
            std::string tags;

            std::regex descrRegex( R"re(\(descr "([^"]*)")re" );
            std::smatch descrMatch;
            if( std::regex_search( content, descrMatch, descrRegex ) )
                description = descrMatch[1].str();

            std::regex tagsRegex( R"re(\(tags "([^"]*)")re" );
            std::smatch tagsMatch;
            if( std::regex_search( content, tagsMatch, tagsRegex ) )
                tags = tagsMatch[1].str();

            std::string mountType;
            if( content.find( "(attr smd)" ) != std::string::npos )
                mountType = "smd";
            else if( content.find( "(attr through_hole)" ) != std::string::npos )
                mountType = "through_hole";

            std::string embedText = name + " " + library + " " + description
                                    + " " + tags + " " + mountType;
            textsToEmbed.push_back( embedText );

            json meta;
            meta["name"] = name;
            meta["library"] = library;
            meta["description"] = description;
            meta["tags"] = tags;
            meta["mounting_type"] = mountType;
            metadatas.push_back( meta );
        }
    }

    if( textsToEmbed.empty() )
        return;

    // Embed all texts in batches
    constexpr size_t BATCH_SIZE = 32;
    std::vector<float> allEmbeddings;
    allEmbeddings.reserve( textsToEmbed.size() * EMBEDDING_DIM );

    for( size_t i = 0; i < textsToEmbed.size(); i += BATCH_SIZE )
    {
        size_t end = std::min( i + BATCH_SIZE, textsToEmbed.size() );
        std::vector<std::string> batch( textsToEmbed.begin() + i, textsToEmbed.begin() + end );
        auto batchEmbeddings = embedQueries( batch );
        allEmbeddings.insert( allEmbeddings.end(), batchEmbeddings.begin(), batchEmbeddings.end() );
    }

    // Insert into index under write lock
    {
        std::unique_lock<std::shared_mutex> lock( m_indexMutex );

        if( idx->index.capacity() < idx->index.size() + textsToEmbed.size() )
            idx->index.reserve( idx->index.size() + textsToEmbed.size() + 1000 );

        for( size_t i = 0; i < textsToEmbed.size(); i++ )
        {
            uint64_t key = nextKey++;
            const float* vec = &allEmbeddings[i * EMBEDDING_DIM];

            // Validate embedding (skip zero vectors)
            float norm = 0.0f;
            for( size_t d = 0; d < EMBEDDING_DIM; d++ )
                norm += vec[d] * vec[d];

            if( norm < 1e-10f )
            {
                wxLogDebug( wxT( "AI DEBUG [VectorSearch]: Skipping zero-vector for: %s" ),
                            wxString( textsToEmbed[i].substr( 0, 50 ) ) );
                continue;
            }

            idx->index.add( key, vec );
            ( *metaMap )[key] = metadatas[i].dump();
        }

        // Save index and metadata
        idx->index.save( idxPath.c_str() );

        std::string metaPath = idxPath + ".meta.json";
        json        metaJson;
        for( auto& [k, v] : *metaMap )
            metaJson[std::to_string( k )] = json::parse( v );

        std::ofstream metaFile( metaPath );
        if( metaFile.is_open() )
            metaFile << metaJson.dump();
    }

    double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0 ).count();

    wxLogDebug( wxT( "AI DEBUG [VectorSearch]: Reindexed %zu items in %.1fms" ),
                textsToEmbed.size(), ms );
}
