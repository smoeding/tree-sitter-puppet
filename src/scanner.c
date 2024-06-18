/**************************************************************************
 *
 * Copyright (c) 2024 Stefan Möding
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
  HEREDOC_BODY,
  HEREDOC_END,
  INTERPOLATION,
  DQ_ESCAPE_SEQUENCE,
};


/**
 * Manage UTF32 strings using an array.
 */

typedef Array(int32_t) UTF32String;


/**
 * The state of the scanner to keep track of the heredoc tag that we are
 * currently looking for.
 */

typedef struct ScannerState {
  UTF32String heredoc_tag;
  bool        interpolation_allowed;
  bool        check_selbrace;
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

    if (lexer->lookahead == U'\'') {
      return has_content;
    }
    else if (lexer->lookahead == U'\\') {
      // Consume backslash and the following character
      lexer->advance(lexer, false);
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

  for(;;) {
    // We are done if the end of file is reached
    if (lexer->eof(lexer)) return false;

    if (lexer->lookahead == U'\n') {
      // No match found when the end of the line is reached
      array_clear(&state->heredoc_tag);
      return false;
    }
    else if (lexer->lookahead == U'"') {
      // The quote character indicates that interpolation can be used
      state->interpolation_allowed = true;
      lexer->advance(lexer, false);
    }
    else if (isalnum(lexer->lookahead)) {
      // We seem to have a heredoc tag, so remember it to find the end
      array_push(&state->heredoc_tag, lexer->lookahead);
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
      return (state->heredoc_tag.size > 0);
    }
    else if (isspace(lexer->lookahead)) {
      // Skip whitespace
      lexer->advance(lexer, true);
    }
    else {
      // That doesn't look like a heredoc tag
      array_clear(&state->heredoc_tag);
      return false;
    }
  }
}


/**
 * Scan a heredoc body. Set result symbol to HEREDOC_END if the tag from the
 * start of the heredoc has been found. Set result symbol to HEREDOC_BODY if
 * the heredoc supports interpolation and a '$' is found. The scanner then
 * uses the scan_interpolation function to continue scanning and returns here
 * later.
 */

static bool scan_heredoc_body(TSLexer *lexer, ScannerState *state) {
  bool     match_valid = true;
  uint32_t match_index = 0;

  for (;;) {
    // The final line might not have a newline at the end but we need to
    // check for the heredoc tag there as well
    if (lexer->eof(lexer)) {
      if (match_valid && (state->heredoc_tag.size == match_index)) {
        array_clear(&state->heredoc_tag);
        lexer->result_symbol = HEREDOC_END;
        return true;
      }

      return false;
    }
    else if (lexer->lookahead == U'\n') {
      // Check for the heredoc tag when the end of the line is reached
      if (match_valid && (state->heredoc_tag.size == match_index)) {
        array_clear(&state->heredoc_tag);
        lexer->result_symbol = HEREDOC_END;
        return true;
      }

      match_valid = true;
      match_index = 0;
      lexer->advance(lexer, false);
    }
    else if (lexer->lookahead == U'$') {
      if (state->interpolation_allowed) {
        // The start of an interpolated value
        lexer->result_symbol = HEREDOC_BODY;
        return true;
      }

      lexer->advance(lexer, false);
    }
    else if (lexer->lookahead == U'\\') {
      // The start of an escape sequences
      lexer->result_symbol = HEREDOC_BODY;
      return true;
    }
    else {
      // Check if the text in the current line matched the heredoc tag.
      // We start if the first alphanumeric character is found to skip
      // whitespace and symbols like '|' or '-'. The comparison continues
      // with the following characters.
      if (match_valid && isalnum(lexer->lookahead)) {
        if ((state->heredoc_tag.size > match_index) &&
            (*array_get(&state->heredoc_tag, match_index) == lexer->lookahead)) {
          // The prefix matches so advance the index to the next tag character
          match_index++;
        }
        else {
          // It doesn't match so skip the rest of the line
          match_valid = false;
        }
      }

      lexer->advance(lexer, false);
    }
  }
}


/**
 * The public interface used by the tree-sitter parser
 */

void *tree_sitter_puppet_external_scanner_create() {
  ScannerState *state = ts_malloc(sizeof(ScannerState));

  if (state) {
    array_init(&state->heredoc_tag);
    state->interpolation_allowed = false;
    state->check_selbrace = false;
  }

  return state;
}

void tree_sitter_puppet_external_scanner_destroy(void *payload) {
  ScannerState *state = (ScannerState*)payload;

  if (state) {
    array_delete(&state->heredoc_tag);
    ts_free(state);
  }
}

unsigned tree_sitter_puppet_external_scanner_serialize(void *payload, char *buffer) {
  ScannerState *state = (ScannerState*)payload;
  unsigned length = 0;  // total size of the serialized data in bytes
  unsigned objsiz;      // size of the current object to serialize

  // Serialize the scanner state. To prevent serialization of each struct
  // item we also include the bogus pointer to the array contents. The
  // pointer will be reset when the object is deserialized (see below).
  objsiz = sizeof(ScannerState);
  memcpy(buffer, payload, objsiz);
  buffer += objsiz;
  length += objsiz;

  // Serialize the array contents.
  objsiz = state->heredoc_tag.capacity * array_elem_size(&state->heredoc_tag);
  if (objsiz > 0) {
    memcpy(buffer, state->heredoc_tag.contents, objsiz);
    buffer += objsiz;
    length += objsiz;
  }

  return length;
}

void tree_sitter_puppet_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  ScannerState *state = (ScannerState*)payload;
  unsigned objsiz;      // size of the current object to deserialize

  // Initialize the structure since the deserialization function will
  // sometimes also be called with length set to zero.
  array_init(&state->heredoc_tag);
  state->interpolation_allowed = false;
  state->check_selbrace = false;

  // Deserialize the scanner state.
  if (length >= sizeof(ScannerState)) {
    objsiz = sizeof(ScannerState);
    memcpy(payload, buffer, objsiz);
    buffer += objsiz;
    length -= objsiz;

    // Check if the array content needs to be deserialized. In this case the
    // contents pointer is invalid as is now contains the address of the
    // contents when serialization has happened. So we need to allocate a new
    // memory chunk and deserialize it there. The size and capacity elements
    // are already defined correctly.
    if (length > 0) {
      state->heredoc_tag.contents = ts_malloc(length);
      memcpy(state->heredoc_tag.contents, buffer, length);
    }
  }
}

bool tree_sitter_puppet_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  ScannerState *state = (ScannerState*)payload;

  if (valid_symbols[QMARK] || valid_symbols[SELBRACE]) {
    return scan_selbrace(lexer, state);
  }

  // First check for an escape sequence or an interpolation and then for
  // a string or heredoc.  The start of an escape sequence or interpolation
  // is easier to spot and only if the lookahead symbol contains something
  // else it will be a regular string.

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

  if (valid_symbols[HEREDOC_BODY] || valid_symbols[HEREDOC_END]) {
    if (state->heredoc_tag.size > 0) {
      return scan_heredoc_body(lexer, state);
    }
  }

  return false;
}
