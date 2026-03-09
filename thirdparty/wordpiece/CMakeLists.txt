# wordpiece - Header-only BERT WordPiece tokenizer for all-MiniLM-L6-v2
# Reads HuggingFace tokenizer.json, produces input_ids + attention_mask

add_library( wordpiece INTERFACE )
target_include_directories( wordpiece INTERFACE ${CMAKE_CURRENT_SOURCE_DIR} )
