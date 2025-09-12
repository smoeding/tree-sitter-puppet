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
  INTERPOLATION_NOBRACE_VARIABLE,
  INTERPOLATION_BRACE_VARIABLE,
  INTERPOLATION_EXPRESSION,
  INTERPOLATION_NOSIGIL_VARIABLE,
  HEREDOC_START,
  HEREDOC_BODY_START,
  HEREDOC_CONTENT,
  HEREDOC_BODY_END,
  HEREDOC_ESCAPE_SEQUENCE,
  DQ_ESCAPE_SEQUENCE,
  SQ_ESCAPE_SEQUENCE,
};

/**
 * heredoc escape flags
 */

const int32_t HEREDOC_ESCAPES[] = {
  U'n',  U'r',  U't',  U's',  U'$', U'u', U'L',
};

/**
 * Manage UTF32 strings using an array.
 */

typedef Array(int32_t) UTF32String;

typedef struct {
  UTF32String word;
  UTF32String indent;
  UTF32String escapes;
  bool        allows_interpolation;
  bool        started;
  bool        end_valid;
} Heredoc;

/**
 * The state of the scanner to keep track of the heredoc tag that we are
 * currently looking for.
 */

typedef struct ScannerState {
  bool           inside_interpolation_variable;
  bool           check_selbrace;
  Array(Heredoc) open_heredocs;
} ScannerState;


/**
 * Helper function to check if a character is valid for a Puppet variable
 * name ('a'..'z', '0'..'9', '_').
 */

static inline bool is_variable_name(int32_t c) {
  return ((isalpha(c) && islower(c)) || isdigit(c) || (c == U'_') || (c == U':'));
}

/**
 * Helper function to check if a character is valid for a Puppet heredoc word
 */

static inline bool is_heredoc_word(int32_t c) {
  return !((c == U':') || (c == U'/') || (c == U'\r') || (c == U'\n') || (c == U')'));
}

/**
 * Helper function to check if a character is valid heredoc escape character
 */

static inline bool is_heredoc_escape(int32_t c) {
  for(size_t i = 0; i < sizeof(HEREDOC_ESCAPES) / sizeof(int32_t); i++)
  {
    if (HEREDOC_ESCAPES[i] == c) {
      return true;
    }
  }
  return false;
}

/**
 * Check if a heredoc escape character is valid for the current heredoc, given
 * the escape flags specified for the heredoc.
 */

static inline bool is_valid_heredoc_escape(UTF32String escapes, int32_t escape) {
  for(size_t i = 0; i < escapes.size; i++) {
    if (escape == *array_get(&escapes, i)) {
      return true;
    }
  }
  return false;
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
 * Scan for the beginning of an interpolation.
 */

static bool scan_interpolation_start(TSLexer *lexer) {
  // The interpolation must start with a '$'
  if (lexer->lookahead != U'$') return false;

  lexer->mark_end(lexer);
  lexer->advance(lexer, false);

  // We are done if the end of file is reached
  if (lexer->eof(lexer)) return false;

  // The style has not been defined yet so this must be the first
  // character after the '$'.
  if (lexer->lookahead == U'{' ||
    is_variable_name(lexer->lookahead))
  {
    return true;
  }
  else {
    // The '$' is not followed by anything that looks like an
    // interpolation.
    return false;
  }
}


/**
 * Scan for an interpolation.
 */

static bool scan_interpolation(TSLexer *lexer, ScannerState *state) {
  // The interpolation must start with a '$'
  if (lexer->lookahead != U'$') return false;

  lexer->mark_end(lexer);
  lexer->advance(lexer, false);

  // We found a possible interpolation, so scanning for a heredoc end word is
  // no longer valid.
  if (state->open_heredocs.size > 0 &&
      state->open_heredocs.contents[0].started &&
      state->open_heredocs.contents[0].allows_interpolation) {
    state->open_heredocs.contents[0].end_valid = false;
  }

  // We are done if the end of file is reached
  if (lexer->eof(lexer)) return false;

  // The style has not been defined yet so this must be the first
  // character after the '$'.
  if (lexer->lookahead == U'{') {
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    if (!is_variable_name(lexer->lookahead)) {
      lexer->result_symbol = INTERPOLATION_EXPRESSION;
      return true;
    }
  }
  else if (is_variable_name(lexer->lookahead)) {
    state->inside_interpolation_variable = true;
    lexer->mark_end(lexer);
    lexer->result_symbol = INTERPOLATION_NOBRACE_VARIABLE;
    return true;
  }
  else {
	// The '$' is not followed by anything that looks like a valid
	// interpolation, but we have already consumed the '$' and it might be the
	// last char in the string or heredoc, so return the appropriate content
	// symbol.
    lexer->mark_end(lexer);
    if (state->open_heredocs.size > 0 &&
        state->open_heredocs.contents[0].started) {
      lexer->result_symbol = HEREDOC_CONTENT;
    } else {
      lexer->result_symbol = DQ_STRING;
    }
    return true;
  }

  for(;;) {
    if (lexer->eof(lexer)) return false;
    if ((lexer->lookahead == U'}') ||
      (lexer->lookahead == U'[') ||
      (lexer->lookahead == U'.')) {
      state->inside_interpolation_variable = true;
      lexer->result_symbol = INTERPOLATION_BRACE_VARIABLE;
      return true;
    }
    else if (!is_variable_name(lexer->lookahead)) {
      lexer->result_symbol = INTERPOLATION_EXPRESSION;
      return true;
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
 * Scan for an escape sequence in a double quoted string.
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
 * Scan for an immediate newline
 */

static bool scan_newline(TSLexer *lexer, bool skip) {
  if (lexer->lookahead == U'\r') {
    lexer->advance(lexer, skip);
    if (lexer->lookahead == U'\n') {
      lexer->advance(lexer, skip);
    } else {
      return false;
    }
  } else if (lexer->lookahead == U'\n') {
    lexer->advance(lexer, skip);
  } else {
    return false;
  }
  return true;
}

/**
 * Scan for a heredoc sequence in a heredoc.
 */

static bool scan_heredoc_escape_sequence(TSLexer *lexer, ScannerState *state) {
  Heredoc *heredoc = array_get(&state->open_heredocs, 0);

  // We are done if the end of file is reached
  if (lexer->eof(lexer)) return false;

  // It's only an escape sequence if it starts with a backslash
  if (lexer->lookahead != U'\\') return false;

  // Mark the end of our token, as we don't know yet whether this is a
  // supported escape sequence
  lexer->mark_end(lexer);
  lexer->advance(lexer, false);

  // We are done if the end of file is reached
  if (lexer->eof(lexer)) return false;

  // Scan for an escaped newline
  if (scan_newline(lexer, false)) {
    heredoc->end_valid = true;
    lexer->mark_end(lexer);
    if (is_valid_heredoc_escape(heredoc->escapes, U'L')) {
      lexer->result_symbol = HEREDOC_ESCAPE_SEQUENCE;
      return true;
    } else {
      lexer->result_symbol = HEREDOC_CONTENT;
      return true;
    }
  }

  // Scan for an escape other than a newline
  if (is_valid_heredoc_escape(heredoc->escapes, lexer->lookahead)) {
    // Scan for Unicode escape sequences: \uXXXX or \u{XXXXXX}
    if (lexer->lookahead == U'u') {
      lexer->advance(lexer, false);
      if (isxdigit((char)lexer->lookahead)) {
        for(size_t i = 0; i < 4 && isxdigit((char)lexer->lookahead); i++) {
          lexer->advance(lexer, false);
        }
      } else if (lexer->lookahead == U'{') {
        lexer->advance(lexer, false);
        for(size_t i = 0; i < 6 && isxdigit((char)lexer->lookahead); i++) {
          lexer->advance(lexer, false);
        }
        if (lexer->lookahead == U'}') {
          lexer->advance(lexer, false);
        }
      }
    } else {
      // Consume single character escape sequence
      lexer->advance(lexer, false);
    }
    lexer->result_symbol = HEREDOC_ESCAPE_SEQUENCE;
  } else {
    lexer->advance(lexer, false);
    lexer->result_symbol = HEREDOC_CONTENT;
  }
  lexer->mark_end(lexer);

  // We found an escape sequnce, so scanning for a heredoc end word is no
  // longer valid.
  heredoc->end_valid = false;
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
 * Scan ahead to see if we have a variable name, following the beginning of
 * an interpolation. Return a zero-width token, indicating a variable is present.
 */

static bool scan_interpolation_nosigil_variable(TSLexer *lexer, ScannerState *state) {
  lexer->result_symbol = INTERPOLATION_NOSIGIL_VARIABLE;

  // Mark the end so we return a zero-width token, and then scan for the name
  // in our normal grammar
  lexer->mark_end(lexer);

  // Update our state, to ensure we don't scan the same
  // characters twice.
  state->inside_interpolation_variable = false;

  for(bool var_found=false;; var_found=true) {
    // We are done if the end of file is reached
    if (lexer->eof(lexer)) return false;

    if (!is_variable_name(lexer->lookahead)) {
      if (var_found) {
        return true;
      }
      else {
        return false;
      }
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
      lexer->mark_end(lexer);
      return has_content;
    }
    // Maybe start of an interpolation
    else if (lexer->lookahead == U'$') {
      if (scan_interpolation_start(lexer)) {
        return has_content;
      }
      else {
        continue;
      }
    }
    else if (lexer->lookahead == U'\\') {
      // Maybe start of an escape sequence
      lexer->mark_end(lexer);
      return has_content;
    }
    lexer->advance(lexer, false);
  }
}


/**
 * Scan from the beginning of a line for a heredoc end tag, return true if
 * found. If heredoc has not already been started, determine whether the
 * heredoc has an indent. If an indent is found, add the indent characters to
 * heredoc.indent.
 */

static bool scan_heredoc_end_tag(TSLexer *lexer, Heredoc *heredoc, bool mark) {
  while (lexer->lookahead == U' ' || lexer->lookahead == U'\t') {
    if (!heredoc->started) {
      array_push(&heredoc->indent, lexer->lookahead);
    }
    lexer->advance(lexer, false);
  }
  if (lexer->lookahead == U'|') {
    lexer->advance(lexer, false);
    while (lexer->lookahead == U' ' || lexer->lookahead == U'\t') {
      lexer->advance(lexer, false);
    }
  } else {
    // heredoc not indented, so clear possible indent chars
    if (! heredoc->started) {
      array_clear(&heredoc->indent);
    }
  }
  if (lexer->lookahead == U'-') {
    lexer->advance(lexer, false);
  }
  while (lexer->lookahead == U' ' || lexer->lookahead == U'\t') {
    lexer->advance(lexer, false);
  }
  size_t position_in_word = 0;
  for(; position_in_word < heredoc->word.size; position_in_word++) {
    if (lexer->lookahead == *array_get(&heredoc->word, position_in_word)) {
      lexer->advance(lexer, false);
    } else {
      break;
    }
  }
  if (position_in_word == heredoc->word.size) {
    // Mark end of possible tag
    if (mark) {
      lexer->mark_end(lexer);
    }
    while (lexer->lookahead == U' ' || lexer->lookahead == U'\t') {
      lexer->advance(lexer, false);
    }
    if (lexer->eof(lexer) || scan_newline(lexer, true)) {
      return true;
    }
  }
  return false;
}

/**
 * Scan for the heredoc end tag, to determine if an indent is present.
 */

static bool scan_heredoc_end_tag_indent(TSLexer *lexer, Heredoc *heredoc) {
  for (;;) {
    if (lexer->eof(lexer)) return false;

    // End tag must occur after a newline
    if (scan_newline(lexer, true)) {
      if (scan_heredoc_end_tag(lexer, heredoc, false)) {
        return true;
      }
    } else {
      lexer->advance(lexer, false);
    }
  }
  return false;
}

/**
 * Scan for a heredoc start tag: @( <endtag> [:<syntax>] [/<escapes>] )
 * The "@(" and ")" tokens are already defined in the grammar, so we do not
 * need to handle those here. The <endtag> token may be enclosed in double
 * quotes to indicate that interpolation is allowed.
 */

static bool scan_heredoc_start(TSLexer *lexer, ScannerState *state) {
  lexer->result_symbol = HEREDOC_START;
  Heredoc heredoc = {0};
  UTF32String word = array_new();
  UTF32String escapes = array_new();

  // We are done if the end of file is reached
  if (lexer->eof(lexer)) return false;

  while (lexer->lookahead == U' ' || lexer->lookahead == U'\t') {
    lexer->advance(lexer, true);
  }
  while (is_heredoc_word(lexer->lookahead)) {
    array_push(&word, lexer->lookahead);
    lexer->advance(lexer, false);
  }
  // The Puppet parser performs a word.rstrip!
  while (word.size > 0 &&
      (*array_get(&word, word.size - 1) == U' ' ||
       *array_get(&word, word.size - 1) == U'\t')) {
    array_erase(&word, word.size - 1);
  }
  if (word.size > 1 && *array_get(&word, 0) == U'"' &&
      *array_get(&word, word.size - 1) == U'"') {
    // The quote character indicates that interpolation can be used
    heredoc.allows_interpolation = true;
    array_erase(&word, 0);
    array_erase(&word, word.size - 1);
  }
  if (word.size == 0) {
    array_delete(&word);
    return false;
  }
  while (lexer->lookahead == U' ' || lexer->lookahead == U'\t') {
    lexer->advance(lexer, true);
  }
  if (lexer->lookahead == U':') {
    lexer->advance(lexer, false);
    // Scan till the end of the syntax file type
    while (isalnum(lexer->lookahead)) {
      lexer->advance(lexer, false);
    }
  }
  while (lexer->lookahead == U' ' || lexer->lookahead == U'\t') {
    lexer->advance(lexer, true);
  }
  if (lexer->lookahead == U'/') {
    lexer->advance(lexer, false);
    // Scan till the end of the escape flags
    while (is_heredoc_escape((char)lexer->lookahead)) {
      array_push(&escapes, lexer->lookahead);
      lexer->advance(lexer, false);
    }
    // We seem to have found a bare '/', so enable all escape sequences
    if (escapes.size == 0) {
      for(size_t i = 0; i < sizeof(HEREDOC_ESCAPES) / sizeof(int32_t); i++)
      {
        array_push(&escapes, HEREDOC_ESCAPES[i]);
      }
    }
    // Add the backslash escape, which is valid for any enabled escape sequence
    array_push(&escapes, U'\\');
  }
  while (lexer->lookahead == U' ' || lexer->lookahead == U'\t') {
    lexer->advance(lexer, true);
  }
  if (lexer->lookahead == U')') {
    // We seem to have found the end of the heredoc tag
    if (word.size > 0) {
      lexer->mark_end(lexer);
      heredoc.word = word;
      heredoc.escapes = escapes;
      heredoc.indent = (UTF32String)array_new();
      if (scan_heredoc_end_tag_indent(lexer, &heredoc)) {
        array_push(&state->open_heredocs, heredoc);
        return true;
      }
    }
  }
  array_delete(&word);
  return false;
}


/**
 * Scan for heredoc content. Set result symbol to HEREDOC_CONTENT if the heredoc
 * supports interpolation and a '$' is found or if an escape sequence is found.
 * The scanner then uses the scan_interpolation or scan_heredoc_escape_sequence
 * to continue scanning and returns here later. However, set the result symbol
 * to HEREDOC_BODY_END if the heredoc end tag is found.
 */

static inline bool scan_heredoc_content(TSLexer *lexer, ScannerState *state) {
  Heredoc *heredoc = array_get(&state->open_heredocs, 0);
  bool has_content = false;

  lexer->mark_end(lexer);
  for (;;) {
    if (lexer->eof(lexer)) return false;

    if (heredoc->end_valid) {
      if (scan_heredoc_end_tag(lexer, heredoc, true)) {
        array_delete(&heredoc->word);
        array_erase(&state->open_heredocs, 0);
        lexer->result_symbol = HEREDOC_BODY_END;
        return true;
      }
      // Check if we have consumed any content
      if (lexer->get_column(lexer) > 0) {
        has_content = true;
      }
      heredoc->end_valid = false;
    }
    // Possible heredoc escape sequence found
    if (lexer->lookahead == U'\\') {
      lexer->mark_end(lexer);
      if (has_content) {
        lexer->result_symbol = HEREDOC_CONTENT;
        return true;
      }
      return false;
    }
    // Possible interpolation
    if (lexer->lookahead == U'$' && heredoc->allows_interpolation) {
      lexer->mark_end(lexer);
      if (has_content) {
        lexer->result_symbol = HEREDOC_CONTENT;
        return true;
      }
      return false;
    }
    if (scan_newline(lexer, false)) {
      heredoc->end_valid = true;
      lexer->mark_end(lexer);
      lexer->result_symbol = HEREDOC_CONTENT;
      return true;
    }
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    has_content = true;
  }
}


/**
 * Scan for the beginning of a heredoc body, following a newline
 */

static inline bool scan_heredoc_body_start(TSLexer *lexer, ScannerState *state) {
  while (lexer->lookahead == U' ' || lexer->lookahead == U'\t') {
    lexer->advance(lexer, true);
  }
  if (scan_newline(lexer, true)) {
    lexer->result_symbol = HEREDOC_BODY_START;
    state->open_heredocs.contents[0].started = true;
    state->open_heredocs.contents[0].end_valid = true;
    return true;
  } else {
    return false;
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
    array_delete(&array_get(&state->open_heredocs, i)->indent);
    array_delete(&array_get(&state->open_heredocs, i)->escapes);
  }
  array_delete(&state->open_heredocs);
  ts_free(state);
}

unsigned tree_sitter_puppet_external_scanner_serialize(void *payload, char *buffer) {
  ScannerState *state = (ScannerState*)payload;
  unsigned size = 0;  // total size of the serialized data in bytes

  buffer[size++] = (char)state->inside_interpolation_variable;
  buffer[size++] = (char)state->check_selbrace;
  buffer[size++] = (char)state->open_heredocs.size;
  for (uint32_t i = 0; i < state->open_heredocs.size; i++) {
    Heredoc *heredoc = array_get(&state->open_heredocs, i);
    if (size + 2 + heredoc->word.size + heredoc->indent.size +
        heredoc->escapes.size >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
      return 0;
    }
    buffer[size++] = (char)heredoc->allows_interpolation;
    buffer[size++] = (char)heredoc->started;
    buffer[size++] = (char)heredoc->end_valid;
    buffer[size++] = (char)heredoc->word.size;
    memcpy(&buffer[size], heredoc->word.contents,
      heredoc->word.size * array_elem_size(&heredoc->word));
    size += heredoc->word.size * array_elem_size(&heredoc->word);
    buffer[size++] = (char)heredoc->indent.size;
    memcpy(&buffer[size], heredoc->indent.contents,
      heredoc->indent.size * array_elem_size(&heredoc->indent));
    size += heredoc->indent.size * array_elem_size(&heredoc->indent);
    buffer[size++] = (char)heredoc->escapes.size;
    memcpy(&buffer[size], heredoc->escapes.contents,
      heredoc->escapes.size * array_elem_size(&heredoc->escapes));
    size += heredoc->escapes.size * array_elem_size(&heredoc->escapes);
  }

  return size;
}

void tree_sitter_puppet_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  ScannerState *state = (ScannerState*)payload;

  // Initialize the structure since the deserialization function will
  // sometimes also be called with length set to zero.
  state->inside_interpolation_variable = false;
  state->check_selbrace = false;
  for (uint32_t i = 0; i < state->open_heredocs.size; i++) {
    array_delete(&array_get(&state->open_heredocs, i)->word);
    array_delete(&array_get(&state->open_heredocs, i)->indent);
    array_delete(&array_get(&state->open_heredocs, i)->escapes);
  }
  array_delete(&state->open_heredocs);

  if (length == 0) {
    return;
  }

  unsigned size = 0;
  state->inside_interpolation_variable = buffer[size++];
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
    memcpy(heredoc.word.contents, &buffer[size],
      word_length * array_elem_size(&heredoc.word));
    heredoc.word.size = word_length;
    size += heredoc.word.size * array_elem_size(&heredoc.word);
    heredoc.indent = (UTF32String)array_new();
    uint8_t indent_length = buffer[size++];
    array_reserve(&heredoc.indent, indent_length);
    memcpy(heredoc.indent.contents, &buffer[size],
      indent_length * array_elem_size(&heredoc.indent));
    heredoc.indent.size = indent_length;
    size += heredoc.indent.size * array_elem_size(&heredoc.indent);
    heredoc.escapes = (UTF32String)array_new();
    uint8_t escapes_length = buffer[size++];
    array_reserve(&heredoc.escapes, escapes_length);
    memcpy(heredoc.escapes.contents, &buffer[size],
      escapes_length * array_elem_size(&heredoc.escapes));
    heredoc.escapes.size = escapes_length;
    size += heredoc.escapes.size * array_elem_size(&heredoc.escapes);
    array_push(&state->open_heredocs, heredoc);
  }

  assert(size == length);
}

bool tree_sitter_puppet_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  ScannerState *state = (ScannerState*)payload;

  if (valid_symbols[HEREDOC_BODY_START]) {
    if (state->open_heredocs.size > 0 &&
        !state->open_heredocs.contents[0].started) {
      if (scan_heredoc_body_start(lexer, state)) {
        return true;
      }
    }
  }

  if (state->open_heredocs.size > 0) {
    Heredoc *heredoc = array_get(&state->open_heredocs, 0);
    if (heredoc->started &&
        heredoc->end_valid &&
        heredoc->indent.size > 0) {
      for(size_t position_in_indent = 0; position_in_indent < heredoc->indent.size; position_in_indent++) {
        if (lexer->lookahead == *array_get(&heredoc->indent, position_in_indent)) {
          lexer->advance(lexer, true);
        }
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

  if (valid_symbols[HEREDOC_ESCAPE_SEQUENCE] && scan_heredoc_escape_sequence(lexer, state)) {
    return true;
  }

  if (valid_symbols[INTERPOLATION_NOSIGIL_VARIABLE] &&
    state->inside_interpolation_variable)
  {
    return scan_interpolation_nosigil_variable(lexer, state);
  }

  if (valid_symbols[INTERPOLATION_NOBRACE_VARIABLE] ||
       valid_symbols[INTERPOLATION_BRACE_VARIABLE] ||
       valid_symbols[INTERPOLATION_EXPRESSION]) {
    if (state->open_heredocs.size == 0 ||
        (state->open_heredocs.contents[0].started &&
        state->open_heredocs.contents[0].allows_interpolation)) {
      if (scan_interpolation(lexer, state)) {
        return true;
      }
    }
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
      return scan_heredoc_content(lexer, state);
    }
  }

  return false;
}
