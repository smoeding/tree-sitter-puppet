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

const PREC = {
  LOW: 5,                       // left
  FARROW: 10,                   // left
  EDGE: 11,                     // left
  EQUALS: 12,                   // right
  AT: 13,                       // right
  OR: 14,                       // left
  AND: 15,                      // left
  QMARK: 16,                    // left
  COMPARE: 17,                  // left
  EQUALITY: 18,                 // left
  SHIFT: 19,                    // left
  ADD: 20,                      // left
  MULTIPLY: 21,                 // left
  MATCH: 22,                    // left
  IN: 23,                       // left
  UMINUS: 24,                   // nonassoc
  SPLAT: 25,                    // nonassoc
  NOT: 26,                      // right
  HIGH: 30,                     // left
};

module.exports = grammar({
  name: 'puppet',

  extras: $ => [
    $.comment,
    /\n/,
    /\r/,
    /\s/,
  ],

  conflicts: $ => [
    [$.resource_type],
  ],

  // The tokens that the external scanner will detect. The order must be the
  // same as defined in the 'TokenType' enum in the scanner.
  externals: $ => [
    $.qmark,
    $.selbrace,
    $._sq_string,
    $._dq_string,
    $._heredoc_start,
    $._heredoc_body,
    $._heredoc_end,
    $.interpolation,
    $.dq_escape_sequence,
  ],

  rules: {
    manifest: $ => optional(
      $._statements,
    ),

    // Collects sequence of elements into a list that the _statements rule can
    // transform (Needed because language supports function calls without
    // parentheses around arguments).

    _statements: $ => choice(
      $.statement,
      seq($._statements, optional(';'), $.statement),
    ),

    block: $ => seq('{', optional($._statements), '}'),

    // This exists to handle multiple arguments to non parenthesized function
    // call. If e is _expression, the a program can consists of e [e,e,e]
    // where the first may be a name of a function to call.

    statement: $ => choice(
      $._assignment,
      $.statement_function,
      seq($.statement, ',', $._assignment),
    ),

    statement_function: $ => seq(
      alias($.statement_function_keywords, $.name),
      choice(
        seq('(', optional($._statement_function_args), ')'),
        alias($._statement_function_args, $.argument_list),
      )
    ),

    _statement_function_args: $ => prec.right(choice(
      alias($._expression, $.argument),
      seq(alias($._expression, $.argument), ',', $._statement_function_args),
    )),

    // Assignment (is right recursive since _assignment is right associative)
    _assignment: $ => prec.right(choice(
      prec(PREC.LOW, $._relationship),
      prec.right(PREC.EQUALS, seq(
        $._relationship,
        field('operator', choice('=', '+=', '-=')),
        $._assignment,
      )),
    )),

    argument: $ => choice(
      $._assignment,
      $.hashpair,
    ),

    _arguments: $ => choice(
      $.argument,
      seq($._arguments, ',', $.argument),
    ),

    argument_list: $ => $._arguments,

    argument_list_comma: $ => seq(
      $._arguments,
      optional(','),
    ),

    _relationship: $ => choice(
      prec(PREC.LOW, $._resource),
      prec.left(PREC.EDGE, seq($._relationship, $.chaining_arrow, $._resource)),
    ),

    chaining_arrow: $ => choice('->', '~>', '<-', '<~'),

    // Resource

    _resource: $ => choice(
      prec(PREC.LOW, $._expression),
      $.resource_type,
      $.resource_reference,
    ),

    // Foo { }
    resource_reference: $ => prec(PREC.HIGH, seq(
      $._resource,
      '{',
      optional(alias($._attribute_operations, $.attribute_list)),
      optional(','),
      '}',
    )),

    // foo { 'title': }
    resource_type: $ => prec(PREC.HIGH, seq(
      optional(choice(alias('@', $.virtual), alias('@@', $.exported))),
      choice($._resource, $._class),
      '{',
      $._resource_bodies,
      optional(';'),
      '}',
    )),

    resource_body: $ => prec.right(seq(
      $.resource_title,
      ':',
      optional(alias($._attribute_operations, $.attribute_list)),
      optional(','),
    )),

    resource_title: $ => $._expression,

    _resource_bodies: $ => choice(
      $.resource_body,
      seq($._resource_bodies, ';', $.resource_body),
    ),

    // Expression

    _expression: $ => choice(
      seq('(', $._assignment, ')'),
      $._primary_expression,
      $._bracketed_expression,
      $.function_call,
      $.unary,
      $.binary,
      prec.left(PREC.QMARK, seq(field('lhs', $._expression), $.selector)),
    ),

    unary: $ => choice(
      prec.right(PREC.NOT,    seq(field('operator', '!'), field('arg', $._expression))),
      prec(      PREC.UMINUS, seq(field('operator', '-'), field('arg', $._expression))),
      prec(      PREC.SPLAT,  seq(field('operator', '*'), field('arg', $._expression))),
    ),

    binary: $ => choice(
      prec.left(PREC.IN,       seq(field('lhs', $._expression), field('operator', 'in'),  field('rhs',    $._expression))),
      prec.left(PREC.MATCH,    seq(field('lhs', $._expression), field('operator', '=~'),  field('rhs',    $._expression))),
      prec.left(PREC.MATCH,    seq(field('lhs', $._expression), field('operator', '!~'),  field('rhs',    $._expression))),
      prec.left(PREC.ADD,      seq(field('lhs', $._expression), field('operator', '+'),   field('rhs',    $._expression))),
      prec.left(PREC.ADD,      seq(field('lhs', $._expression), field('operator', '-'),   field('rhs',    $._expression))),
      prec.left(PREC.MULTIPLY, seq(field('lhs', $._expression), field('operator', '/'),   field('rhs',    $._expression))),
      prec.left(PREC.MULTIPLY, seq(field('lhs', $._expression), field('operator', '*'),   field('rhs',    $._expression))),
      prec.left(PREC.MULTIPLY, seq(field('lhs', $._expression), field('operator', '%'),   field('rhs',    $._expression))),
      prec.left(PREC.SHIFT,    seq(field('lhs', $._expression), field('operator', '<<'),  field('rhs',    $._expression))),
      prec.left(PREC.SHIFT,    seq(field('lhs', $._expression), field('operator', '>>'),  field('rhs',    $._expression))),
      prec.left(PREC.EQUALITY, seq(field('lhs', $._expression), field('operator', '!='),  field('rhs',    $._expression))),
      prec.left(PREC.EQUALITY, seq(field('lhs', $._expression), field('operator', '=='),  field('rhs',    $._expression))),
      prec.left(PREC.COMPARE,  seq(field('lhs', $._expression), field('operator', '>'),   field('rhs',    $._expression))),
      prec.left(PREC.COMPARE,  seq(field('lhs', $._expression), field('operator', '>='),  field('rhs',    $._expression))),
      prec.left(PREC.COMPARE,  seq(field('lhs', $._expression), field('operator', '<'),   field('rhs',    $._expression))),
      prec.left(PREC.COMPARE,  seq(field('lhs', $._expression), field('operator', '<='),  field('rhs',    $._expression))),
      prec.left(PREC.AND,      seq(field('lhs', $._expression), field('operator', 'and'), field('rhs',    $._expression))),
      prec.left(PREC.OR,       seq(field('lhs', $._expression), field('operator', 'or'),  field('rhs',    $._expression))),
    ),

    _bracketed_expression: $ => seq(
      $._expression,
      token.immediate('['),
      alias($._access_elements, $.access),
      optional(','),
      ']',
    ),

    _access_elements: $ => choice(
      $.access_element,
      seq($._access_elements, ',', $.access_element),
    ),

    access_element: $ => choice(
      $._expression,
      $.hashpair,
    ),

    // Expressions

    _expressions: $ => seq(
      $._expression,
      repeat(seq(',', $._expression)),
    ),

    _primary_expression: $ => prec.left(choice(
      $.variable,
      $.call_method_with_lambda,
      $.resource_collector,
      $.case,
      $.if,
      $.unless,
      $.define_definition,
      $.class_definition,
      $.plan_definition,
      $.apply_expression,
      $.node_definition,
      $.function_definition,
      $.type_alias,
      $.type_definition,
      $.reserved_word,
      $.array,
      $.hash,
      $.regex,
      $._quotedtext,
      $.type,
      $.number,
      $._boolean,
      $.default,
      $.undef,
      $.name,
    )),

    // Call Function

    function_call: $ => prec.right(PREC.HIGH, seq(
      choice($._expression, $.type),
      '(',
      optional(alias($.argument_list_comma, $.argument_list)),
      ')',
      optional($.lambda),
    )),

    // Call Method

    call_method_with_lambda: $ => prec.right(choice(
      $.call_method,
      seq($.call_method, $.lambda),
    )),

    call_method: $ => prec.right(choice(
      seq($.named_access, '(', $.argument_list, ')'),
      seq($.named_access, '(', ')'),
      $.named_access,
    )),

    named_access: $ => seq(
      $._expression,
      field('operator', token.immediate('.')),
      choice($.name, $.type),
    ),

    // Lambda

    lambda: $ => seq(
      alias($._lambda_parameter_list, $.parameter_list),
      optional($.return_type),
      $.block,
    ),

    _lambda_parameter_list: $ => seq(
      '|',
      optional(seq($._parameters, optional(','))),
      '|',
    ),

    // Conditionals

    // If

    if: $ => seq(
      'if',
      alias($._expression, $.condition),
      $.block,
      repeat($.elsif),
      optional($.else),
    ),

    elsif: $ => seq('elsif', alias($._expression, $.condition), $.block),
    else: $ => seq('else', $.block),

    // Unless

    unless: $ => seq(
      'unless',
      alias($._expression, $.condition),
      $.block,
      optional($.else),
    ),

    // Case Expression

    case: $ => seq('case', $._expression, '{', $._case_options, '}'),

    _case_options: $ => choice(
      $.case_option,
      seq($._case_options, $.case_option),
    ),

    case_option: $ => prec(PREC.HIGH, seq( // Higher than $._expression
      $._expressions,
      ':',
      $.block,
    )),

    // This special construct is required or racc will produce the wrong
    // result when the selector entry LHS is generalized to any _expression
    // (LBRACE looks like a hash). Thus it is not possible to write
    // a selector with a single entry where the entry LHS is a hash.  The
    // SELBRACE token is a LBRACE that follows a QMARK, and this is produced
    // by the lexer with a lookback

    selector: $ => seq(
      $.qmark,
      $.selbrace,
      $._selector_option_list,
      optional(','),
      '}',
    ),

    _selector_option_list: $ => choice(
      $.selector_option,
      seq($._selector_option_list, ',', $.selector_option),
    ),

    selector_option: $ => prec.left(PREC.FARROW, seq(
      field('name', $._expression),
      '=>',
      field('value', $._expression),
    )),

    // Collection

    resource_collector: $ => prec.right(seq(
      seq($._expression, $.collect_query),
      optional(seq(
        '{',
        optional(alias($._attribute_operations, $.attribute_list)),
        optional(','),
        '}',
      )),
    )),

    collect_query: $ => choice(
      seq('<|', optional($._expression), '|>'),
      seq('<<|', optional($._expression), '|>>'),
    ),

    // Attribute Operations (Not an _expression)

    _attribute_operations: $ => choice(
      $.attribute,
      seq($._attribute_operations, ',', $.attribute),
    ),

    attribute: $ => choice(
      seq(field('name', $._attribute_name), alias('=>', $.arrow), field('value',$._expression)),
      seq(field('name', $._attribute_name), alias('+>', $.arrow), field('value',$._expression)),
      seq(field('name', '*'), alias('=>', $.arrow), field('value',$._expression)),
    ),

    _attribute_name: $ => choice($.name, $.keyword),

    // Define

    define_definition: $ => seq(
      'define',
      $.classname,
      optional($.parameter_list),
      $.block,
    ),

    // Plan

    plan_definition: $ => seq(
      'plan',
      $.classname,
      optional($.parameter_list),
      $.block,
    ),

    apply_expression: $ => seq(
      'apply',
      '(',
      alias($.argument_list_comma, $.argument_list),
      ')',
      $.block
    ),

    // Hostclass

    class_definition: $ => seq(
      'class',
      $.classname,
      optional($.parameter_list),
      optional(seq('inherits', choice($.classname, $.default))),
      $.block,
    ),

    // Node

    node_definition: $ => seq(
      'node',
      $._hostnames,
      optional(','),
      optional(seq('inherits', $.hostname)),
      $.block,
    ),

    // Hostnames is not a list of names, it is a list of name matchers
    // (including a Regexp).

    _hostnames: $ => choice(
      $.hostname,
      seq($._hostnames, ',', $.hostname),
    ),

    hostname: $ => choice(
      $.dotted_name,
      $._quotedtext,
      $.default,
      $.regex,
    ),

    dotted_name: $ => choice(
      $.name_or_number,
      seq($.dotted_name, token.immediate('.'), $.name_or_number),
    ),

    name_or_number: $ => choice(
      $.name,
      $.number,
    ),

    // Function Definition

    function_definition: $ => seq(
      'function',
      $.classname,
      optional($.parameter_list),
      optional($.return_type),
      $.block,
    ),

    return_type: $ => seq(
      '>>',
      $.type,
      optional(seq(
        token.immediate('['),
        alias($._access_elements, $.access),
        ']',
      ))
    ),

    // Names and parameters common to several rules

    classname: $ => choice(
      $.name,
      $.type,
      // $.word,   // does not seem to be valid here
    ),

    parameter_list: $ => choice(
      seq('(', ')'),
      seq('(', $._parameters, optional(','), ')'),
    ),

    _parameters: $ => choice(
      $.parameter,
      seq($._parameters, ',', $.parameter),
    ),

    parameter: $ => choice($._untyped_parameter, $.typed_parameter),

    _untyped_parameter: $ => choice($.regular_parameter, $.splat_parameter),

    regular_parameter: $ => choice(
      seq($.variable, field('operator', '='), $._expression),
      $.variable,
    ),

    splat_parameter: $ => seq('*', $.regular_parameter),

    typed_parameter: $ => seq($._parameter_type, $._untyped_parameter),

    _parameter_type: $ => choice(
      $.type,
      seq(
        $.type,
        token.immediate('['),
        alias($._access_elements, $.access),
        ']',
      ),
    ),

    // Type Alias

    type_alias: $ => prec.right(seq(
      $._type_alias_lhs,
      field('operator', '='),
      choice(
        seq(
          $.type,
          $.hash
        ),
        seq(
          $.type,
          token.immediate('['),
          alias($._access_elements, $.access),
          optional(','),
          ']'
        ),
        $.type,
        $.hash,
        $.array,
      ),
    )),

    _type_alias_lhs: $ => seq('type', $._parameter_type),

    // Type Definition

    type_definition: $ => seq(
      'type',
      $.type,
      optional(seq('inherits', $.type)),
      $.block,
    ),

    // Variable

    variable: $ => seq('$', alias($._variable_name, $.name)),

    _variable_name: _ => token.immediate(/(::)?(\w+::)*\w+/),

    // Reserved words

    reserved_word: $ => choice('private', 'attr'),

    // Literals (dynamic and static)

    _quotedtext: $ => choice(
      $.single_quoted_string,
      $.double_quoted_string,
      $.heredoc
    ),

    single_quoted_string: $ => seq(
      "'",
      repeat($._sq_string),
      "'",
    ),

    double_quoted_string: $ => seq(
      '"',
      repeat(
        choice($._dq_string,
          $.interpolation,
          alias($.dq_escape_sequence, $.escape_sequence),
        ),
      ),
      '"',
    ),

    heredoc: $ => seq(
      '@(',
      $._heredoc_start,
      ')',
      repeat(
        choice(
          $._heredoc_body,
          $.interpolation,
          alias($.dq_escape_sequence, $.escape_sequence),
        ),
      ),
      $._heredoc_end,
    ),

    // The '#' is valid inside a regex and doesn't start a comment here so we
    // need to include that here.
    regex: $ => seq(
      '/',
      field('pattern', repeat(choice(
        '#',
        $._regex_char,
        $._regex_char_escaped))
      ),
      '/',
    ),

    _regex_char: $ => token.immediate(/[^/\\\n\r]/),

    _regex_char_escaped: $ => token.immediate(seq('\\', /./)),

    array: $ => seq(
      '[',
      optional(seq($._collection_entries, optional(','))),
      ']',
    ),

    _collection_entries: $ => choice(
      $._collection_entry,
      seq($._collection_entries, ',', $._collection_entry),
    ),

    _collection_entry: $ => choice(
      alias($.argument, $.array_element),
      alias($.collection_entry_keyword, $.array_element),
    ),

    hash: $ => choice(
      seq('{', $._hashpairs, '}'),
      seq('{', $._hashpairs, ',', '}'),
      seq('{', '}'),
    ),

    _hashpairs: $ => choice(
      $.hashpair,
      seq($._hashpairs, ',', $.hashpair),
    ),

    hashpair: $ => seq(
      field('key', $._hash_entry),
      alias('=>', $.arrow),
      field('value', $._hash_entry)
    ),

    _hash_entry: $ => seq(
      optional('_'),            // Note: originally not in the Puppet grammar
      choice($._assignment, $.collection_entry_keyword)
    ),

    // Keywords valid as a collection value
    collection_entry_keyword: $ => choice('type', 'function'),

    keyword: _ => choice(
      'and', 'case', 'class', 'default', 'define', 'else', 'elsif', 'if', 'in',
      'inherits', 'node', 'or', 'undef', 'unless', 'type', 'attr', 'function',
      'private', 'plan', 'apply',
    ),

    statement_function_keywords: _ => choice(
      'include', 'require', 'contain', 'tag',      // Catalog statements
      'debug', 'info', 'notice', 'warning', 'err', // Logging statements
      'fail',                                      // Failure statements
    ),

    type:     _ => /((::){0,1}[A-Z]\w*)+/,
    name:     _ => /((::)?[a-z]\w*)(::[a-z]\w*)*/,
    word:     _ => /((?:::){0,1}(?:[a-z_](?:[\w-]*\w)?))+/,

    // handle the class keyword like a name node
    _class:   $ => alias('class', $.name),

    number:   _ => /(?:0[xX][0-9A-Fa-f]+|0?\d+(?:\.\d+)?(?:[eE]-?\d+)?)/,
    default:  _ => 'default',
    undef:    _ => 'undef',
    _boolean: $ => choice($.true, $.false),
    true:     _ => 'true',
    false:    _ => 'false',

    comment: _ => seq('#', /.*/),
  },
})
