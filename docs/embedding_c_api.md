# C API policy and typed data for embedders

These additive APIs are declared in `src/include/c_api/lbug.h`. Existing string,
prepare, query and error-message APIs retain their behavior.

## Inspect syntax before preparing

Call `lbug_parse_statement(text, &info)` before preparing untrusted text. It
requires exactly one statement and returns its original `info.type`. Check
`info.has_load_from` as well: a file scan is a clause inside a QUERY, including
UNION branches and query parts. EXPLAIN/PROFILE retain their own statement kind.

This call needs no database and does not bind, execute, start a transaction,
resolve catalog names, or invoke extension transformers or standalone-call
rewriters. Unknown tables/functions can therefore parse successfully. Failed
inspection leaves the type UNKNOWN. Do not interpret that as permission.

The regular `ClientContext::parseQuery` helper is not suitable for this check:
it rewrites standalone calls by binding their table functions. Preparing first
and inspecting the prepared type is too late to prevent binding-time effects.

## Function policy

`lbug_connection_set_function_allowlist` copies case-insensitive function names.
The default is unrestricted. An empty list denies explicit function calls;
`lbug_connection_clear_function_allowlist` restores the default.

The binder checks explicit scalar, aggregate, macro/rewrite, standalone and
in-query table-function calls before their arguments or bind callbacks. Macro
bodies also go through binding. Parser-lowered syntax uses engine function names:
for example, COUNT(*) requires COUNT_STAR, addition requires '+', and unary
negation requires NEGATE. An empty list rejects those forms too. Expressions
bound for schema defaults, including SERIAL's NEXTVAL, also require their function
names. Low-level binder helpers such as inserted type-coercion casts do not
re-enter parsed-expression binding and are not checked as additional calls.

Choose the actual names as part of the embedder's dialect contract. Aliases are
separate spellings: permitting one does not permit an unlisted alias. An allowed
function and its implementation must be trusted. This mechanism does not replace
statement policy, filesystem restrictions, resource limits, or result validation.

Every successful policy change invalidates previously prepared handles on that
connection. Executing one fails with a BINDER category and requires a new prepare.
This also covers constant-folded functions and cached rewritten/physical plans.
Handles cannot execute on a different connection. Other connections are unaffected.
An invalid policy input leaves the old policy
intact.

## Length-aware strings

The `*_string_with_length` create/bind/get variants use byte lengths, not character
counts. They copy embedded NULs and do not require input terminators. A null input
pointer is allowed only with length zero.

The getter requires a non-null STRING value. Its owned buffer has a trailing NUL
in addition to the returned byte length. Use `lbug_destroy_string` to free it and
`lbug_value_destroy` for created values. Supplied getter outputs are null/zero
on failure. These APIs do not validate UTF-8; that is the embedder's responsibility.

## Errors

Prepared statements and query results retain an error category independently of
their message. `lbug_prepared_statement_get_error_category` and
`lbug_query_result_get_error_category` return NONE on success, or PARSER, BINDER,
RUNTIME, CONSTRAINT, INTERRUPT, or IO on failure. Invalid handles return RUNTIME.

Categories come from exception types, not message text. WAL/checkpoint wrappers
retain nested causes while preserving their existing messages and base catches.
A later refusal caused by an already-poisoned WAL remains RUNTIME. Constraint subclasses
preserve the existing runtime/COPY message prefixes and base-class catches.
Unclassified exceptions, including buffer-pool-full and allocation failures, are
RUNTIME. A failed prepare's category survives an attempted execute.

Use `lbug_get_last_error_category` on the failing thread for errors without a
result, such as parsing or opening. Taking the message does not consume its
category. Calls that clear the last-error message clear the category too; the
result/statement getters do not depend on this thread-local slot. Existing
argument checks that return an error without recording one do not populate it.
Use the returned status and object-specific category when an object is available.

An IO category alone does not decide whether a transaction committed or whether
an operating-system error means disk exhaustion. The embedder must track operation
and commit state; this API never guesses an outcome.

## Parameter names

`lbug_prepared_statement_get_parameter_count` and
`lbug_prepared_statement_get_parameter_name` enumerate distinct names in
lexicographic order, without '$', on a successful prepare. Each name is an owned
string freed with `lbug_destroy_string`.

The enumeration includes both known and unresolved parameters: the latter have
no value/type at the first C API prepare. Extra names supplied by binding calls
are not statement parameters. Compare the enumeration with supplied parameters
before executing to detect missing or unused names.

## Tests

`test/c_api/embedder_test.cpp` uses an empty database without COPY fixtures and
runs under both the default engine configuration and the malloc-buffer-manager,
external-access-disabled embedding configuration. Its additional COPY endpoint
case is skipped when external access is disabled. Run it as part of `c_api_test`;
binder changes also require the API/end-to-end suites.
