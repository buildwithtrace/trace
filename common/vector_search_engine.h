#ifndef VECTOR_SEARCH_ENGINE_H
#define VECTOR_SEARCH_ENGINE_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <wx/string.h>

// Forward-declare ONNX Runtime types to avoid exposing the header everywhere
namespace Ort {
class Env;
class Session;
class SessionOptions;
class MemoryInfo;
} // namespace Ort

namespace wordpiece {
class Tokenizer;
}


/**
 * Singleton engine for local vector search over KiCad symbol/footprint libraries.
 *
 * Replaces the Python subprocess-per-call approach with a warm in-process
 * ONNX Runtime session + usearch HNSW index.  Typical search latency drops
 * from ~370 ms (Python cold-start) to ~12 ms (warm C++).
 *
 * Thread safety:
 *   - Search() acquires a shared (read) lock on the index.
 *   - ReindexStaleFiles() acquires an exclusive (write) lock.
 *   - Multiple concurrent searches are allowed; reindex blocks searches
 *     only for the brief window while the index is swapped.
 */
class VECTOR_SEARCH_ENGINE
{
public:
    static VECTOR_SEARCH_ENGINE& GetInstance();

    struct SearchResult
    {
        std::string id;
        std::string metadataJson;
        float       score;
    };

    /**
     * Lazy-initialize the engine.  Loads the ONNX model, tokenizer, and
     * usearch indices.  Safe to call multiple times; subsequent calls are
     * no-ops if already initialized.
     *
     * @param aModelPath     Path to all-MiniLM-L6-v2.onnx
     * @param aTokenizerPath Path to tokenizer.json
     * @param aIndexDir      Directory containing *.usearch index files and metadata.db
     * @param aSymbolLibDirs Colon-separated symbol library directories
     * @param aFootprintLibDirs Colon-separated footprint library directories
     * @return true on success
     */
    bool Init( const wxString& aModelPath,
               const wxString& aTokenizerPath,
               const wxString& aIndexDir,
               const wxString& aSymbolLibDirs,
               const wxString& aFootprintLibDirs );

    bool IsInitialized() const { return m_initialized.load(); }

    /**
     * Search the index for the given queries.
     *
     * @param aMode    "symbol" or "footprint"
     * @param aQueries List of search query strings
     * @param aTopK    Number of results per query
     * @return Vector of results (aQueries.size() * aTopK entries, grouped by query)
     */
    std::string Search( const std::string& aMode,
                        const std::vector<std::string>& aQueries,
                        int aTopK );

    /**
     * Check whether the index is stale (library files changed on disk)
     * and kick off a background re-index if needed.
     */
    void CheckFreshnessAndReindex( const std::string& aMode,
                                   const std::vector<std::string>& aLibDirs );

    /**
     * Re-index specific files.  Runs in the calling thread (intended to
     * be dispatched from a background thread).
     */
    void ReindexFiles( const std::string& aMode,
                       const std::vector<std::string>& aFiles );

private:
    VECTOR_SEARCH_ENGINE();
    ~VECTOR_SEARCH_ENGINE();

    VECTOR_SEARCH_ENGINE( const VECTOR_SEARCH_ENGINE& ) = delete;
    VECTOR_SEARCH_ENGINE& operator=( const VECTOR_SEARCH_ENGINE& ) = delete;

    /**
     * Embed a batch of query strings into 384-d float vectors using ONNX Runtime.
     * @return Matrix of shape [queries.size(), 384] in row-major order.
     */
    std::vector<float> embedQueries( const std::vector<std::string>& aQueries );

    /**
     * L2-normalize a vector in-place.
     */
    static void l2Normalize( float* vec, size_t dim );

    // -- ONNX Runtime state (warm, loaded once) --
    struct OrtState;
    std::unique_ptr<OrtState>               m_ort;

    // -- Tokenizer (warm, loaded once) --
    std::unique_ptr<wordpiece::Tokenizer>   m_tokenizer;

    // -- Index state (per-mode) --
    struct IndexData;
    std::unique_ptr<IndexData>              m_symbolIndex;
    std::unique_ptr<IndexData>              m_footprintIndex;

    // Protects index reads/writes
    mutable std::shared_mutex               m_indexMutex;

    // Metadata lookup: usearch key -> JSON string
    std::unordered_map<uint64_t, std::string> m_symbolMeta;
    std::unordered_map<uint64_t, std::string> m_footprintMeta;

    // Library paths for freshness checks
    std::string m_symbolLibDirs;
    std::string m_footprintLibDirs;
    std::string m_indexDir;

    std::atomic<bool> m_initialized{ false };
    std::atomic<bool> m_reindexRunning{ false };
    std::mutex        m_initMutex;
};

#endif // VECTOR_SEARCH_ENGINE_H
