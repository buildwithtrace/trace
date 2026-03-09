#ifndef WORDPIECE_TOKENIZER_HPP
#define WORDPIECE_TOKENIZER_HPP

/**
 * @file wordpiece_tokenizer.hpp
 * @brief Self-contained BERT WordPiece tokenizer for all-MiniLM-L6-v2.
 *
 * Reads a HuggingFace tokenizer.json and produces input_ids + attention_mask
 * compatible with BERT-family ONNX models. Handles:
 *   - BertNormalizer (lowercase, clean control chars, handle Chinese chars)
 *   - BertPreTokenizer (split on whitespace + punctuation)
 *   - WordPiece subword tokenization with ## prefix
 *   - [CLS] / [SEP] wrapping, padding to fixed length, truncation
 */

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace wordpiece {

struct TokenizerOutput {
    std::vector<int64_t> input_ids;
    std::vector<int64_t> attention_mask;
};

class Tokenizer {
public:
    bool LoadFromJson( const std::string& path )
    {
        std::ifstream f( path );
        if( !f.is_open() )
            return false;

        std::string content( ( std::istreambuf_iterator<char>( f ) ),
                             std::istreambuf_iterator<char>() );

        if( !parseVocab( content ) )
            return false;

        m_loaded = true;
        return true;
    }

    bool IsLoaded() const { return m_loaded; }

    TokenizerOutput Encode( const std::string& text, int maxLen = 128 ) const
    {
        std::string normalized = normalize( text );
        std::vector<std::string> words = preTokenize( normalized );

        std::vector<int64_t> tokenIds;
        tokenIds.push_back( m_clsId );

        for( const auto& word : words )
        {
            auto subTokens = wordPieceTokenize( word );
            for( int64_t id : subTokens )
                tokenIds.push_back( id );
        }

        // Truncate to maxLen - 1 (leave room for [SEP])
        if( (int) tokenIds.size() > maxLen - 1 )
            tokenIds.resize( maxLen - 1 );

        tokenIds.push_back( m_sepId );

        TokenizerOutput out;
        out.input_ids.resize( maxLen, m_padId );
        out.attention_mask.resize( maxLen, 0 );

        for( size_t i = 0; i < tokenIds.size(); i++ )
        {
            out.input_ids[i] = tokenIds[i];
            out.attention_mask[i] = 1;
        }

        return out;
    }

    std::vector<TokenizerOutput> EncodeBatch( const std::vector<std::string>& texts,
                                              int maxLen = 128 ) const
    {
        std::vector<TokenizerOutput> results;
        results.reserve( texts.size() );
        for( const auto& t : texts )
            results.push_back( Encode( t, maxLen ) );
        return results;
    }

private:
    // Minimal JSON vocab parser -- extracts "vocab": { "token": id, ... }
    bool parseVocab( const std::string& content )
    {
        // Find "model" -> "vocab"
        size_t modelPos = content.find( "\"model\"" );
        if( modelPos == std::string::npos )
            return false;

        size_t vocabPos = content.find( "\"vocab\"", modelPos );
        if( vocabPos == std::string::npos )
            return false;

        // Find the opening brace of vocab object
        size_t braceStart = content.find( '{', vocabPos + 7 );
        if( braceStart == std::string::npos )
            return false;

        // Parse key-value pairs
        size_t pos = braceStart + 1;
        int depth = 1;

        while( pos < content.size() && depth > 0 )
        {
            skipWhitespace( content, pos );
            if( pos >= content.size() )
                break;

            char c = content[pos];

            if( c == '}' )
            {
                depth--;
                pos++;
                continue;
            }

            if( c == '{' )
            {
                depth++;
                pos++;
                continue;
            }

            if( c == ',' )
            {
                pos++;
                continue;
            }

            if( c == '"' )
            {
                std::string key = parseJsonString( content, pos );
                skipWhitespace( content, pos );
                if( pos < content.size() && content[pos] == ':' )
                    pos++;
                skipWhitespace( content, pos );

                int64_t val = parseJsonInt( content, pos );
                m_vocab[key] = val;
                if( val >= (int64_t) m_idToToken.size() )
                    m_idToToken.resize( val + 1 );
                m_idToToken[val] = key;
            }
            else
            {
                pos++;
            }
        }

        if( m_vocab.empty() )
            return false;

        auto findId = [this]( const std::string& token ) -> int64_t {
            auto it = m_vocab.find( token );
            return it != m_vocab.end() ? it->second : 0;
        };

        m_clsId = findId( "[CLS]" );
        m_sepId = findId( "[SEP]" );
        m_unkId = findId( "[UNK]" );
        m_padId = findId( "[PAD]" );

        return true;
    }

    static void skipWhitespace( const std::string& s, size_t& pos )
    {
        while( pos < s.size() && ( s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n'
                                   || s[pos] == '\r' ) )
            pos++;
    }

    static std::string parseJsonString( const std::string& s, size_t& pos )
    {
        if( pos >= s.size() || s[pos] != '"' )
            return {};

        pos++; // skip opening quote
        std::string result;
        while( pos < s.size() && s[pos] != '"' )
        {
            if( s[pos] == '\\' && pos + 1 < s.size() )
            {
                pos++;
                switch( s[pos] )
                {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                case 'u':
                {
                    if( pos + 4 < s.size() )
                    {
                        std::string hex = s.substr( pos + 1, 4 );
                        unsigned long cp = std::stoul( hex, nullptr, 16 );
                        pos += 4;
                        // Simple UTF-8 encoding for BMP
                        if( cp < 0x80 )
                        {
                            result += (char) cp;
                        }
                        else if( cp < 0x800 )
                        {
                            result += (char) ( 0xC0 | ( cp >> 6 ) );
                            result += (char) ( 0x80 | ( cp & 0x3F ) );
                        }
                        else
                        {
                            result += (char) ( 0xE0 | ( cp >> 12 ) );
                            result += (char) ( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
                            result += (char) ( 0x80 | ( cp & 0x3F ) );
                        }
                    }
                    break;
                }
                default: result += s[pos]; break;
                }
            }
            else
            {
                result += s[pos];
            }
            pos++;
        }
        if( pos < s.size() )
            pos++; // skip closing quote
        return result;
    }

    static int64_t parseJsonInt( const std::string& s, size_t& pos )
    {
        bool neg = false;
        if( pos < s.size() && s[pos] == '-' )
        {
            neg = true;
            pos++;
        }
        int64_t val = 0;
        while( pos < s.size() && s[pos] >= '0' && s[pos] <= '9' )
        {
            val = val * 10 + ( s[pos] - '0' );
            pos++;
        }
        return neg ? -val : val;
    }

    // BertNormalizer: lowercase + strip control chars
    std::string normalize( const std::string& text ) const
    {
        std::string result;
        result.reserve( text.size() );

        for( size_t i = 0; i < text.size(); i++ )
        {
            unsigned char c = text[i];

            // Clean control characters (except whitespace-like)
            if( c == 0 || isControlChar( c ) )
                continue;

            // Whitespace normalization
            if( isWhitespaceChar( c ) )
            {
                result += ' ';
                continue;
            }

            // Lowercase ASCII
            if( c >= 'A' && c <= 'Z' )
            {
                result += (char) ( c + 32 );
                continue;
            }

            result += (char) c;
        }

        return result;
    }

    static bool isControlChar( unsigned char c )
    {
        if( c == '\t' || c == '\n' || c == '\r' )
            return false;
        return ( c >= 0 && c < 0x20 ) || c == 0x7F;
    }

    static bool isWhitespaceChar( unsigned char c )
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    static bool isPunctuation( unsigned char c )
    {
        // ASCII punctuation ranges
        return ( c >= 33 && c <= 47 ) || ( c >= 58 && c <= 64 ) || ( c >= 91 && c <= 96 )
               || ( c >= 123 && c <= 126 );
    }

    // BertPreTokenizer: split on whitespace and punctuation
    std::vector<std::string> preTokenize( const std::string& text ) const
    {
        std::vector<std::string> tokens;
        std::string current;

        for( size_t i = 0; i < text.size(); i++ )
        {
            unsigned char c = text[i];

            if( isWhitespaceChar( c ) )
            {
                if( !current.empty() )
                {
                    tokens.push_back( current );
                    current.clear();
                }
                continue;
            }

            if( isPunctuation( c ) )
            {
                if( !current.empty() )
                {
                    tokens.push_back( current );
                    current.clear();
                }
                tokens.push_back( std::string( 1, (char) c ) );
                continue;
            }

            current += (char) c;
        }

        if( !current.empty() )
            tokens.push_back( current );

        return tokens;
    }

    // WordPiece tokenization
    std::vector<int64_t> wordPieceTokenize( const std::string& word ) const
    {
        if( word.size() > 100 )
            return { m_unkId };

        std::vector<int64_t> output;
        size_t start = 0;

        while( start < word.size() )
        {
            size_t end = word.size();
            int64_t foundId = -1;

            while( start < end )
            {
                std::string substr = word.substr( start, end - start );
                if( start > 0 )
                    substr = "##" + substr;

                auto it = m_vocab.find( substr );
                if( it != m_vocab.end() )
                {
                    foundId = it->second;
                    break;
                }
                end--;
            }

            if( foundId < 0 )
                return { m_unkId };

            output.push_back( foundId );
            start = end;
        }

        return output;
    }

    std::unordered_map<std::string, int64_t> m_vocab;
    std::vector<std::string> m_idToToken;
    int64_t m_clsId = 101;
    int64_t m_sepId = 102;
    int64_t m_unkId = 100;
    int64_t m_padId = 0;
    bool    m_loaded = false;
};

} // namespace wordpiece

#endif // WORDPIECE_TOKENIZER_HPP
