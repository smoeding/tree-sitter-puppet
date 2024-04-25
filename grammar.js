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
    $._fixed_string,
    $._expandable_string,
    $._heredoc_start,
    $._heredoc_body,
    $._heredoc_end,
    $.interpolation,
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

    _statement_body: $ => seq('{', optional($._statements), '}'),

    // This exists to handle multiple arguments to non parenthesized function
    // call. If e is _expression, the a program can consists of e [e,e,e]
    // where the first may be a name of a function to call.

    statement: $ => choice(
      $._assignment,
      seq($.statement, ',', $._assignment),
    ),

    // Assignment (is right recursive since _assignment is right associative)
    _assignment: $ => prec.right(choice(
      prec(PREC.LOW, $._relationship),
      prec.right(PREC.EQUALS, seq(
        $._relationship,
        alias(choice('=', '+=', '-='), $.operator),
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

    argument_list: $ => seq(
      $._arguments,
    ),

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

      // foo { 'title': }
      $.resource_type,

      // Foo { }
      prec(PREC.HIGH, seq(
        alias($._resource, $.resource_reference),
        '{',
        optional($.attribute_operations),
        optional(','),
        '}',
      )),
    ),

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
      optional($.attribute_operations),
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
      prec.left(PREC.QMARK, seq(field('left', $._expression), $.selector)),
    ),

    unary: $ => choice(
      prec.right(PREC.NOT,    seq('!', field('argument', $._expression))),
      prec(      PREC.UMINUS, seq('-', field('argument', $._expression))),
      prec(      PREC.SPLAT,  seq('*', field('argument', $._expression))),
    ),

    binary: $ => choice(
      prec.left(PREC.IN,       seq(field('left', $._expression), alias('in',  $.operator), field('right',    $._expression))),
      prec.left(PREC.MATCH,    seq(field('left', $._expression), alias('=~',  $.operator), field('right',    $._expression))),
      prec.left(PREC.MATCH,    seq(field('left', $._expression), alias('!~',  $.operator), field('right',    $._expression))),
      prec.left(PREC.ADD,      seq(field('left', $._expression), alias('+',   $.operator), field('right',    $._expression))),
      prec.left(PREC.ADD,      seq(field('left', $._expression), alias('-',   $.operator), field('right',    $._expression))),
      prec.left(PREC.MULTIPLY, seq(field('left', $._expression), alias('/',   $.operator), field('right',    $._expression))),
      prec.left(PREC.MULTIPLY, seq(field('left', $._expression), alias('*',   $.operator), field('right',    $._expression))),
      prec.left(PREC.MULTIPLY, seq(field('left', $._expression), alias('%',   $.operator), field('right',    $._expression))),
      prec.left(PREC.SHIFT,    seq(field('left', $._expression), alias('<<',  $.operator), field('right',    $._expression))),
      prec.left(PREC.SHIFT,    seq(field('left', $._expression), alias('>>',  $.operator), field('right',    $._expression))),
      prec.left(PREC.EQUALITY, seq(field('left', $._expression), alias('!=',  $.operator), field('right',    $._expression))),
      prec.left(PREC.EQUALITY, seq(field('left', $._expression), alias('==',  $.operator), field('right',    $._expression))),
      prec.left(PREC.COMPARE,  seq(field('left', $._expression), alias('>',   $.operator), field('right',    $._expression))),
      prec.left(PREC.COMPARE,  seq(field('left', $._expression), alias('>=',  $.operator), field('right',    $._expression))),
      prec.left(PREC.COMPARE,  seq(field('left', $._expression), alias('<',   $.operator), field('right',    $._expression))),
      prec.left(PREC.COMPARE,  seq(field('left', $._expression), alias('<=',  $.operator), field('right',    $._expression))),
      prec.left(PREC.AND,      seq(field('left', $._expression), alias('and', $.operator), field('right',    $._expression))),
      prec.left(PREC.OR,       seq(field('left', $._expression), alias('or',  $.operator), field('right',    $._expression))),
    ),

    _bracketed_expression: $ => seq(
      $._expression,
      token.immediate('['),
      $._access_args,
      optional(','),
      ']',
    ),

    _access_args: $ => choice(
      $.access_arg,
      seq($._access_args, ',', $.access_arg),
    ),

    access_arg: $ => choice(
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
      $.collection_expression,
      $.case_expression,
      $.if_expression,
      $.unless_expression,
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
      alias(token.immediate('.'), $.operator),
      choice($.name, $.type),
    ),

    // Lambda

    lambda: $ => seq(
      alias($._lambda_parameter_list, $.parameter_list),
      optional($.return_type),
      $.lambda_body,
    ),

    lambda_body: $ => $._statement_body,

    _lambda_parameter_list: $ => seq(
      '|',
      optional(seq($._parameters, optional(','))),
      '|',
    ),

    // Conditionals

    // If

    //if_expression: $ => seq('if', $._if_part),
    if_expression: $ => seq(
      'if',
      $._expression,
      alias($._statement_body, $.body),
      repeat($.elsif_clause),
      optional($.else_clause),
    ),

    elsif_clause: $ => seq('elsif', $._expression, alias($._statement_body, $.body)),
    else_clause: $ => seq('else', alias($._statement_body, $.body)),

    // Unless

    unless_expression: $ => seq(
      'unless',
      $._expression,
      alias($._statement_body, $.body),
      optional($.else_clause),
    ),

    // Case Expression

    case_expression: $ => seq('case', $._expression, '{', $._case_options, '}'),

    _case_options: $ => choice(
      $.case_option,
      seq($._case_options, $.case_option),
    ),

    case_option: $ => prec(PREC.HIGH, seq( // Higher than $._expression
      $._expressions,
      ':',
      alias($._statement_body, $.body),
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

    collection_expression: $ => prec.right(seq(
      seq($._expression, $.collect_query),
      optional(seq(
        '{',
        optional($.attribute_operations),
        optional(','),
        '}',
      )),
    )),

    collect_query: $ => choice(
      seq('<|', optional($._expression), '|>'),
      seq('<<|', optional($._expression), '|>>'),
    ),

    // Attribute Operations (Not an _expression)

    attribute_operations: $ => choice(
      $.attribute,
      seq($.attribute_operations, ',', $.attribute),
    ),

    attribute: $ => choice(
      seq(field('name', $._attribute_name), '=>', field('value',$._expression)),
      seq(field('name', $._attribute_name), '+>', field('value',$._expression)),
      seq(field('name', '*'), '=>', field('value',$._expression)),
    ),

    _attribute_name: $ => choice($.name, $.keyword),

    // Define

    define_definition: $ => seq(
      'define',
      $.classname,
      optional($.parameter_list),
      $._statement_body,
    ),

    // Plan

    plan_definition: $ => seq(
      'plan',
      $.classname,
      optional($.parameter_list),
      $._statement_body,
    ),

    apply_expression: $ => seq(
      'apply',
      '(',
      alias($.argument_list_comma, $.argument_list),
      ')',
      $._statement_body
    ),

    // Hostclass

    class_definition: $ => seq(
      'class',
      $.classname,
      optional($.parameter_list),
      optional(seq('inherits', choice($.classname, $.default))),
      $._statement_body,
    ),

    // Node

    node_definition: $ => seq(
      'node',
      $._hostnames,
      optional(','),
      optional(seq('inherits', $.hostname)),
      $._statement_body,
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
      alias($._statement_body, $.function_body),
    ),

    return_type: $ => choice(
      seq('>>', $.type),
      seq('>>', $.type, token.immediate('['), $._access_args, ']'),
    ),

    // Names and parameters common to several rules

    classname: $ => choice(
      $.name,
      $.word,
      $.classref,
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
      seq($.variable, '=', $._expression),
      $.variable,
    ),

    splat_parameter: $ => seq('*', $.regular_parameter),

    typed_parameter: $ => seq($._parameter_type, $._untyped_parameter),

    _parameter_type: $ => choice(
      $.type,
      seq($.type, token.immediate('['), $._access_args, ']'),
    ),

    // Type Alias

    type_alias: $ => prec.right(choice(
      seq($._type_alias_lhs, alias('=', $.operator), $.type, $.hash),
      seq($._type_alias_lhs, alias('=', $.operator), $.type, token.immediate('['), $._access_args, optional(','), ']'),
      seq($._type_alias_lhs, alias('=', $.operator), $.type),
      seq($._type_alias_lhs, alias('=', $.operator), $.hash),
      seq($._type_alias_lhs, alias('=', $.operator), $.array),
    )),

    _type_alias_lhs: $ => seq('type', $._parameter_type),

    // Type Definition

    type_definition: $ => seq(
      'type',
      $.classref,
      optional(seq('inherits', $.classref)),
      $._statement_body,
    ),

    // Variable

    variable: $ => seq('$', alias($._variable_name, $.name)),

    _variable_name: _ => token.immediate(/(::)?(\w+::)*\w+/),

    // Reserved words

    reserved_word: $ => choice('private', 'attr'),

    // Literals (dynamic and static)

    _quotedtext: $ => choice($.string, $.heredoc),

    string: $ => choice(
      seq("'", repeat($._fixed_string), "'"),
      seq('"', repeat(choice($._expandable_string, $.interpolation)), '"'),
    ),

    heredoc: $ => seq(
      '@(',
      $._heredoc_start,
      ')',
      choice(
        $._heredoc_end,
        seq(
          repeat(choice(
            $._heredoc_body,
            $.interpolation,
          )),
          $._heredoc_end,
        ),
      ),
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
      '=>',
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

    classref: _ => /((::){0,1}[A-Z]\w*)+/,
    type:     $ => $.classref,
    name:     _ => /((::)?[a-z]\w*)(::[a-z]\w*)*/,
    word:     _ => /((?:::){0,1}(?:[a-z_](?:[\w-]*\w)?))+/,

    // handle the class keyword like a name node
    _class:    $ => alias('class', $.name),

    number:   _ => /(?:0[xX][0-9A-Fa-f]+|0?\d+(?:\.\d+)?(?:[eE]-?\d+)?)/,
    default:  _ => 'default',
    undef:    _ => 'undef',
    _boolean: $ => choice($.true, $.false),
    true:     _ => 'true',
    false:    _ => 'false',

    comment: _ => seq('#', /.*/),
  },
})
