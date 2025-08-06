/**************************************************************************
 *
 * Copyright (c) 2024, 2025 Stefan Möding
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 **************************************************************************/


#include <stdlib.h>
#include <ctype.h>

#include "tree_sitter/parser.h"
#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"


/**
 * The tokens that this scanner will detect. The order must be the same as
 * defined in the 'externals' field in the grammar.
 */

enum TokenType {
  QMARK,
  SELBRACE,
  SQ_STRING,
  DQ_STRING,
  HEREDOC_START,
  HEREDOC_BODY_START,
  HEREDOC_CONTENT,
  HEREDOC_BODY_END,
  INTERPOLATION,
  DQ_ESCAPE_SEQUENCE,
  SQ_ESCAPE_SEQUENCE,
};


/**
 * Manage UTF32 strings using an array.
 */

typedef Array(int32_t) UTF32String;

typedef struct {
  UTF32String word;
  bool allows_interpolation;
  bool started;
  bool end_valid;
} Heredoc;

/**
 * The state of the scanner to keep track of the heredoc tag that we are
 * currently looking for.
 */

typedef struct ScannerState {
  bool           check_selbrace;
  Array(Heredoc) open_heredocs;
} ScannerState;


/**
 * Helper function to check if a character is valid for a Puppet variable
 * name ('a'..'z', '0'..'9', '_').
 */

static inline bool is_variable_name(int32_t c) {
  return ((isalpha(c) && islower(c)) || isdigit(c) || (c == U'_'));
}


/**
 * Scan for the opening brace after a question mark to detect a selector.
 */

static bool scan_selbrace(TSLexer *lexer, ScannerState *state) {
  for(;;) {
    // We are done if the end of file is reached
    if (lexer->eof(lexer)) return false;

    if (isspace(lexer->lookahead) || (lexer->lookahead == U'\n')) {
      // Skip whitespace
      lexer->advance(lexer, true);
    }
    else if (lexer->lookahead == U'?') {
      state->check_selbrace = true;
      lexer->result_symbol = QMARK;
      lexer->advance(lexer, false);
      return true;
    }
    else if (lexer->lookahead == U'{') {
      if (state->check_selbrace) {
        state->check_selbrace = false;
        lexer->result_symbol = SELBRACE;
        lexer->advance(lexer, false);
        return true;
      }

      return false;
    }
    else {
      state->check_selbrace = false;
      return false;
    }
  }
}


/**
 * Scan for content of single and double quoted strings. The function uses
 * the lookahead value as the start token and scans until the same token is
 * found again.
 */

static bool skip_quoted_string(TSLexer *lexer) {
  int32_t quote = lexer->lookahead;

  lexer->advance(lexer, false);

  for(;;) {
    // We are done if the end of file is reached
    if (lexer->eof(lexer)) return false;

    if (lexer->lookahead == quote) {
      return true;
    }
    else if (lexer->lookahead == U'\\') {
      // Consume backslash and the following character
      lexer->advance(lexer, false);
    }

    lexer->advance(lexer, false);
  }
}


/**
 * The string/heredoc interpolation styles we recognize. The style is used to
 * decide when the interpolation should end.
 */

enum InterpolationStyle {
  NONE,     // initial value before the style has been detected
  BRACE,    // ${foo}
  VARIABLE, // $foo
};


/**
 * Scan for an interpolation.
 */

static bool scan_interpolation(TSLexer *lexer) {
  enum InterpolationStyle style = NONE;

  // The interpolation must start with a '$'
  if (lexer->lookahead != U'$') return false;

  lexer->result_symbol = INTERPOLATION;
  lexer->advance(lexer, false);

  for(;;) {
    // We are done if the end of file is reached
    if (lexer->eof(lexer)) return false;

    switch (style) {
    case NONE:
      // The style has not been defined yet so this must be the first
      // character after the '$'.
      if (lexer->lookahead == U'{') {
        style = BRACE;
      }
      else if (is_variable_name(lexer->lookahead)) {
        style = VARIABLE;
      }
      else {
        // The '$' is not followed by anything that looks like an
        // interpolation. The '$' has been already been consumed and
        // therefore will be treated as whatever the lexer finds next
        // (presumably a string content or a heredoc).
        return false;
      }
      break;

    case BRACE:
      if (lexer->lookahead == U'}') {
        lexer->advance(lexer, false);
        return true;
      }
      else if ((lexer->lookahead == U'\'') || (lexer->lookahead == U'"')) {
        if (!skip_quoted_string(lexer)) {
          // Skipping the quoted string failed. This happens if the string is
          // missing the terminating quote.
          return false;
        }
      }
      break;

    case VARIABLE:
      if (!is_variable_name(lexer->lookahead)) {
        // Return if the end of a valid variable name is reached.
        return true;
      }
      break;
    }

    lexer->advance(lexer, false);
  }
}


/**
 * Scan for an escape sequence in a single quoted string.
 */

static bool scan_sq_escape_sequence(TSLexer *lexer) {
  // We are done if the end of file is reached
  if (lexer->eof(lexer)) return false;

  // It's only an escape sequence if it starts with a backslash
  if (lexer->lookahead != U'\\') return false;

  lexer->advance(lexer, false);

  // We are done if the end of file is reached
  if (lexer->eof(lexer)) return false;

  // There are two allowed escape sequences in a single quoted string: the
  // single backslash and the literal single quotation mark. We return
  // a match if we find one of these and otherwise indicate a not-found
  // condition. In this case the parser tries to match a normal string
  // instead. This should succeed since we already consumed the initial
  // backslash. It will be treated as a normal character just like the
  // documentation states.
  // https://www.puppet.com/docs/puppet/latest/lang_data_string.html

  if ((lexer->lookahead != U'\\') && (lexer->lookahead != U'\'')) {
    return false;
  }

  // The following character belongs to the escape sequence
  lexer->advance(lexer, false);

  lexer->result_symbol = SQ_ESCAPE_SEQUENCE;
  return true;
}


/**
 * Scan for an escape sequence in a double quoted string or heredoc.
 */

static bool scan_dq_escape_sequence(TSLexer *lexer) {
  // We are done if the end of file is reached
  if (lexer->eof(lexer)) return false;

  // It's only an escape sequence if it starts with a backslash
  if (lexer->lookahead != U'\\') return false;

  lexer->advance(lexer, false);

  // We are done if the end of file is reached
  if (lexer->eof(lexer)) return false;

  // The following character belongs to the escape sequence
  lexer->advance(lexer, false);

  lexer->result_symbol = DQ_ESCAPE_SEQUENCE;
  return true;
}


/**
 * Scan over a single quoted string. Interpolation is not recognized in this
 * type of string.
 */

static bool scan_sq_string(TSLexer *lexer) {
  lexer->result_symbol = SQ_STRING;

  for(bool has_content=false;; has_content=true) {
    // We are done if the end of file is reached
    if (lexer->eof(lexer)) return false;

    if ((lexer->lookahead == U'\'') || (lexer->lookahead == U'\\')) {
      return has_content;
    }

    lexer->advance(lexer, false);
  }
}


/**
 * Scan over a double quoted string. Interpolation is possible in this type
 * of string. If the '$' is detected we return to indicate that the string
 * content ends and the function scan_interpolation will continue.
 */

static bool scan_dq_string(TSLexer *lexer) {
  lexer->result_symbol = DQ_STRING;

  for(bool has_content=false;; has_content=true) {
    // We are done if the end of file is reached
    if (lexer->eof(lexer)) return false;

    if (lexer->lookahead == U'"') {
      return has_content;
    }
    else if (lexer->lookahead == U'$') {
      // Maybe start of an interpolation
      return has_content;
    }
    else if (lexer->lookahead == U'\\') {
      // Maybe start of an escape sequence
      return has_content;
    }
    lexer->advance(lexer, false);
  }
}


/**
 * Scan for a heredoc start tag: @( <endtag> [:<syntax>] [/<escapes>] )
 * The "@(" and ")" tokens are already defined in the grammar, so we do not
 * need to handle these here. The <endtag> token may be enclosed in double
 * quotes to indicate that interpolation is allowed.
 */

static bool scan_heredoc_start(TSLexer *lexer, ScannerState *state) {
  lexer->result_symbol = HEREDOC_START;
  Heredoc heredoc = {0};
  UTF32String word = array_new();

  for(;;) {
    // We are done if the end of file is reached
    if (lexer->eof(lexer)) return false;

    if (lexer->lookahead == U'\n') {
      // No match found when the end of the line is reached
      array_delete(&word);
      return false;
    }
    else if (lexer->lookahead == U'"') {
      // The quote character indicates that interpolation can be used
      heredoc.allows_interpolation = true;
      lexer->advance(lexer, false);
    }
    else if (isalnum(lexer->lookahead)) {
      // We seem to have a heredoc tag, so remember it to find the end
      array_push(&word, lexer->lookahead);
      lexer->advance(lexer, false);
    }
    else if (lexer->lookahead == U':') {
      // Scan till the end of the syntax flags
      lexer->advance(lexer, false);
      while (isalnum(lexer->lookahead)) {
        lexer->advance(lexer, false);
      }
    }
    else if (lexer->lookahead == U'/') {
      // Scan till the end of the escape flags
      lexer->advance(lexer, false);
      while (isalpha(lexer->lookahead)) {
        lexer->advance(lexer, false);
      }
    }
    else if (lexer->lookahead == U')') {
      // We seem to have found the end of the heredoc tag
      if (word.size > 0) {
        heredoc.word = word;
        array_push(&state->open_heredocs, heredoc);
        return (word.size > 0);
      } else {
        array_delete(&word);
        return false;
      }
    }
    else if (isspace(lexer->lookahead)) {
      // Skip whitespace
      lexer->advance(lexer, true);
    }
    else {
      // That doesn't look like a heredoc tag
      array_delete(&word);
      return false;
    }
  }
}


/**
 * Scan a heredoc body. Set result symbol to HEREDOC_BODY_END if the tag from the
 * start of the heredoc has been found. Set result symbol to HEREDOC_CONTENT if
 * the heredoc supports interpolation and a '$' is found. The scanner then
 * uses the scan_interpolation function to continue scanning and returns here
 * later.
 */

static inline bool scan_heredoc_content(ScannerState *state, TSLexer *lexer) {
  Heredoc *heredoc = array_get(&state->open_heredocs, 0);
  bool has_content = false;

  lexer->mark_end(lexer);
  for (;;) {
    if (lexer->eof(lexer)) return false;

    if (heredoc->end_valid) {
      while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
        lexer->advance(lexer, false);
      }
      if ( lexer->lookahead == '|' ) {
        lexer->advance(lexer, false);
      }
      while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
        lexer->advance(lexer, false);
      }
      if (lexer->lookahead == '-') {
        lexer->advance(lexer, false);
      }
      while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
        lexer->advance(lexer, false);
      }
      size_t position_in_word = 0;
      for(; position_in_word < heredoc->word.size; position_in_word++) {
        if (lexer->lookahead == *array_get(&heredoc->word, position_in_word)) {
          lexer->advance(lexer, false);
        }
      }
      if (position_in_word == heredoc->word.size) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
          lexer->advance(lexer, false);
        }
        if (lexer->eof(lexer) ||
          lexer->lookahead == '\n' ||
          lexer->lookahead == '\r')
        {
          lexer->mark_end(lexer);
          array_delete(&heredoc->word);
          array_erase(&state->open_heredocs, 0);
          lexer->result_symbol = HEREDOC_BODY_END;
          return true;
        }
      }
      heredoc->end_valid = false;
    } else {
      heredoc->end_valid = false;
      /* TODO: parse start tag to determine which escape flags are allowed */
      if (lexer->lookahead == '\\') {
        lexer->mark_end(lexer);
        if (has_content) {
          lexer->result_symbol = HEREDOC_CONTENT;
          return true;
        }
        return false;
      }
      if (heredoc->allows_interpolation && lexer->lookahead == '$') {
        lexer->mark_end(lexer);
        if (has_content) {
          lexer->result_symbol = HEREDOC_CONTENT;
          return true;
        }
        return false;
      } else if (lexer->lookahead == '\r' || lexer->lookahead == '\n') {
        if (lexer->lookahead == '\r') {
          lexer->advance(lexer, false);
          if (lexer->lookahead == '\n') {
            lexer->advance(lexer, false);
          }
        } else {
          lexer->advance(lexer, false);
        }
        heredoc->end_valid = true;
        lexer->mark_end(lexer);
        lexer->result_symbol = HEREDOC_CONTENT;
        return true;
      } else {
        has_content = true;
        lexer->advance(lexer, false);
        lexer->mark_end(lexer);
      }
    }
  }
}


/**
 * The public interface used by the tree-sitter parser
 */

void *tree_sitter_puppet_external_scanner_create() {
  ScannerState *state = ts_malloc(sizeof(ScannerState));
  array_init(&state->open_heredocs);
  return state;
}

void tree_sitter_puppet_external_scanner_destroy(void *payload) {
  ScannerState *state = (ScannerState*)payload;
  for (uint32_t i = 0; i < state->open_heredocs.size; i++) {
    array_delete(&array_get(&state->open_heredocs, i)->word);
  }
  array_delete(&state->open_heredocs);
  ts_free(state);
}

unsigned tree_sitter_puppet_external_scanner_serialize(void *payload, char *buffer) {
  ScannerState *state = (ScannerState*)payload;
  unsigned size = 0;  // total size of the serialized data in bytes

  buffer[size++] = (char)state->check_selbrace;
  buffer[size++] = (char)state->open_heredocs.size;
  for (uint32_t i = 0; i < state->open_heredocs.size; i++) {
    Heredoc *heredoc = array_get(&state->open_heredocs, i);
    if (size + 2 + heredoc->word.size >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
      return 0;
    }
    buffer[size++] = (char)heredoc->allows_interpolation;
    buffer[size++] = (char)heredoc->started;
    buffer[size++] = (char)heredoc->end_valid;
    buffer[size++] = (char)heredoc->word.size;
    memcpy(&buffer[size], heredoc->word.contents, heredoc->word.size * array_elem_size(&heredoc->word));
    size += heredoc->word.size * array_elem_size(&heredoc->word);
  }

  return size;
}

void tree_sitter_puppet_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  ScannerState *state = (ScannerState*)payload;

  // Initialize the structure since the deserialization function will
  // sometimes also be called with length set to zero.
  state->check_selbrace = false;
  for (uint32_t i = 0; i < state->open_heredocs.size; i++) {
    array_delete(&array_get(&state->open_heredocs, i)->word);
  }
  array_delete(&state->open_heredocs);

  if (length == 0) {
    return;
  }

  unsigned size = 0;
  state->check_selbrace = buffer[size++];
  uint8_t open_heredoc_count = buffer[size++];
  for (unsigned j = 0; j < open_heredoc_count; j++) {
    Heredoc heredoc = {0};
    heredoc.allows_interpolation = buffer[size++];
    heredoc.started = buffer[size++];
    heredoc.end_valid = buffer[size++];
    heredoc.word = (UTF32String)array_new();
    uint8_t word_length = buffer[size++];
    array_reserve(&heredoc.word, word_length);
    memcpy(heredoc.word.contents, &buffer[size], word_length * array_elem_size(&heredoc.word));
    heredoc.word.size = word_length;
    size += heredoc.word.size * array_elem_size(&heredoc.word);
    array_push(&state->open_heredocs, heredoc);
  }

  assert(size == length);
}

static inline bool scan_heredoc_body_start(TSLexer *lexer, void *payload) {
  ScannerState *state = (ScannerState*)payload;

  for (;;) {
    switch (lexer->lookahead) {
      case ' ':
      case '\t':
      case '\r':
        lexer->advance(lexer, true);
        break;
      case '\n':
        lexer->advance(lexer, true);
        lexer->result_symbol = HEREDOC_BODY_START;
        state->open_heredocs.contents[0].started = true;
        state->open_heredocs.contents[0].end_valid = true;
        return true;
      default:
        return false;
    }
  }
}

bool tree_sitter_puppet_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  ScannerState *state = (ScannerState*)payload;

  if (valid_symbols[HEREDOC_BODY_START]) {
    if (state->open_heredocs.size > 0 &&
        !state->open_heredocs.contents[0].started)
    {
      if (scan_heredoc_body_start(lexer, state)) {
        return true;
      }
    }
  }

  if (valid_symbols[QMARK] || valid_symbols[SELBRACE]) {
    return scan_selbrace(lexer, state);
  }

  // First check for an escape sequence or an interpolation and then for
  // a string or heredoc. The start of an escape sequence or interpolation is
  // easier to spot and only if the lookahead symbol contains something else
  // it will be a regular string.

  if (valid_symbols[SQ_ESCAPE_SEQUENCE] && scan_sq_escape_sequence(lexer)) {
    return true;
  }

  if (valid_symbols[DQ_ESCAPE_SEQUENCE] && scan_dq_escape_sequence(lexer)) {
    return true;
  }

  if (valid_symbols[INTERPOLATION] && scan_interpolation(lexer)) {
    return true;
  }

  if (valid_symbols[DQ_STRING]) {
    return scan_dq_string(lexer);
  }

  if (valid_symbols[SQ_STRING]) {
    return scan_sq_string(lexer);
  }

  if (valid_symbols[HEREDOC_START]) {
    return scan_heredoc_start(lexer, state);
  }

  if (valid_symbols[HEREDOC_CONTENT] || valid_symbols[HEREDOC_BODY_END]) {
    if (state->open_heredocs.size > 0) {
      return scan_heredoc_content(state, lexer);
    }
  }

  return false;
}
