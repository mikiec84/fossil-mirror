# Fossil grep vs POSIX grep

As of Fossil 2.7, there is a `grep` command which acts roughly like
POSIX's `grep -E` over all historical versions of a single file name.
This document explains the commonalities and divergences between [POSIX
`grep`](http://pubs.opengroup.org/onlinepubs/9699919799/utilities/grep.html)
and Fossil `grep`.


## Options

Fossil `grep` supports only a small subset of the options specified for
POSIX `grep`:

| Option | Meaning
|--------|-------------------------------------------------------------
| `-i`   | ignore case in matches
| `-l`   | list a checkin ID prefix for matching historical versions of the file
| `-v`   | print each checkin ID considered, regardless of whether it matches

That leaves many divergences at the option level from POSIX `grep`:

*   There is no built-in way to get a count of matches, as with `grep
    -c`.

*    You cannot give more than one pattern, as with `grep -e` or `grep
     -f`.

*   There is no equivalent of `grep -F` to do literal fixed-string
    matches only.

*   `fossil grep -l` does not do precisely the same thing as POSIX
    `grep -l`: it lists checkin ID prefixes, not file names.

*   Fossil always gives the line number in its output, which is to say
    that it acts like `grep -n`.  There is no way to disable the line
    number in `fossil grep` output.

*   There is no way to suppress all output, returning only a status code
    to indicate whether the pattern matched, as with `grep -q`.

*   There is no way to suppress error output, as with `grep -s`.

*   Fossil `grep` accepts only a single input file name. You cannot give
    it a list of file names, and you cannot give it a directory name for
    Fossil to expand to the set of all files under that directory. This
    means Fossil `grep` has no equivalent of the common POSIX `grep -R`
    extension. (And if it did, it would probably have a different option
    letter, since `-R` in Fossil has a different meaning, by
    convention.)

*   You cannot invert the match, as with `grep -v`.

Patches to remove those limitations will be thoughtfully considered.


## Regular Expression Dialect

Fossil contains a built-in regular expression engine implementing a
subset of the [POSIX extended regular expression][ere] dialect:

[ere]: https://en.wikipedia.org/wiki/Regular_expression#POSIX_extended

| Atom    | Meaning
|---------|-------------------------------------------------------------
| `X*`    | zero or more occurrences of X
| `X+`    | one or more occurrences of X
| `X?`    | zero or one occurrences of X
| `X{p,q}`| between p and q occurrences of X, inclusive
| `(X)`   | match X
| <tt>X\|Y</tt>| X or Y
| `^X`    | X occurring at the beginning of a line
| `X$`    | X occurring at the end of a line
| `.`     | Match any single character
| `\c`    | Character `c` where `c` is one of <tt>{}()[]\|\*+?.\\</tt>
| `\c`    | C-language escapes for `c` in `afnrtv`.  ex: `\t` or `\n`
| `\uXXXX`| Where XXXX is exactly 4 hex digits, Unicode value XXXX
| `\xXX`  | Where XX is exactly 2 hex digits, Unicode value XX
| `[abc]` | Any single character from the set `abc`
| `[^abc]`| Any single character not in the set `abc`
| `[a-z]` | Any single character in the range `a-z`
| `[^a-z]`| Any single character not in the range `a-z`
| `\b`    | Word boundary
| `\w`    | Word character: `[A-Za-z0-9_]`
| `\W`    | Non-word character: `[^A-Za-z0-9_]`
| `\d`    | Digit: `[0-9]`
| `\D`    | Non-digit: `[^0-9]`
| `\s`    | Whitespace character: `[ \t\r\n\v\f]`
| `\S`    | Non-whitespace character: `[^ \t\r\n\v\f]`

There are several restrictions in Fossil `grep` relative to a fully
POSIX compatible regular expression engine. Among them are:

*   There is currently no support for POSIX character classes such as
    `[:lower:]`.

*   Fossil `grep` does not currently attempt to take your operating
    system's locale settings into account when doing this match.  Since
    Fossil has no way to mark a given file as having a particular
    encoding, Fossil `grep` assumes the input files are in UTF-8 format.

    This means Fossil `grep` will not work correctly if the files in
    your repository are in an encoding that is not backwards-compatible
    with ASCII, such as UTF-16.  Partially compatible encodings such as
    ISO 8859 should work with Fossil `grep` as long as you stick to
    their ASCII-compatible subset.

The Fossil `grep` language is not a strict subset of POSIX extended
regular expressions.  Some of the features documented above are
well-understood extensions to it, such as the "word" features `\b`, `\w`
and `\W`.

Fossil `grep` is based on the Unicode engine from [SQLite's FTS5
feature][fts5].  This means it does do things like Unicode-aware case
folding. Therefore, it is usually a user error to attempt to substitute
`[a-z]` for a lack of the POSIX character class `[:lower:]` if you are
grepping over pretty much any human written language other than English.
Use `fossil grep -i` instead, which does Unicode case folding.

[fts5]: https://www.sqlite.org/fts5.html


## Algorithm Details

Fossil `grep` uses a [nondeterministic finite automaton][nfa] for
matching, so the performance is bounded by ***O(nm)*** where ***n*** is
the size of the regular expression and ***m*** is the size of the input
string.

[nfa]: https://en.wikipedia.org/wiki/Nondeterministic_finite_automaton

In order to avoid [ReDoS attacks][redos], the Fossil regular expression
matcher was purposely written to avoid [implementation choices][rei]
which have the potential to require exponential evaluation time. This
constrains the possible feature set we can support in the Fossil `grep`
dialect. For instance, we are unlikely to ever add support for
[backtracking][bt].

[redos]: https://en.wikipedia.org/wiki/ReDoS
[rei]:   https://en.wikipedia.org/wiki/Regular_expression#Implementations_and_running_times
[bt]:    https://en.wikipedia.org/wiki/Backtracking

The `X{p,q}` operator expands to `p` copies of `X` followed by `q-p`
copies of `X?` before RE evaluation. The ***O(nm)*** performance bound
above remains true for this case, but realize that it applies to the RE
*after* this expansion, not to the form as given by the user.  In other
words, as `q-p` increases, so does the RE evaluation time.
