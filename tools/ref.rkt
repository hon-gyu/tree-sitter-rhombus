#lang racket/base
;; Print the official shrubbery parse of a file in the normalized form that
;; tools/ts2shrub.py produces from tree-sitter, so the two can be diffed.
;; Usage: racket tools/ref.rkt FILE ...
;; Prints one line per file: FILE, a tab, then the parse or `ERROR: message`.
(require racket/port
         racket/string
         shrubbery/parse)

(define (normalize d)
  (cond
    [(pair? d)
     (case (car d)
       [(op) (format "(op ~a)" (cadr d))]
       ;; adjacent strings are merged, since tools/ts2shrub.py only
       ;; approximates how at-notation text is split into strings
       [(brackets) (format "(~a)" (string-join (collapse (map normalize d)) " "))]
       [else (format "(~a)" (string-join (map normalize d) " "))])]
    [(symbol? d) (format "~a" d)]
    [(keyword? d) (format "~~~a" (keyword->string d))]
    [(number? d) "num"]
    [(string? d) "str"]
    [(bytes? d) "bytes"]
    [(boolean? d) "bool"]
    [(void? d) "void"]
    [(char? d) "char"]
    [else (format "?~s" d)]))

(define (collapse items)
  (for/fold ([out '()] #:result (reverse out))
            ([item (in-list items)])
    (if (and (equal? item "(group str)") (pair? out) (equal? (car out) "(group str)"))
        out
        (cons item out))))

(define (parse-file path)
  (define text (call-with-input-file path port->string))
  ;; blank out a `#lang` line so positions stay the same
  (define body (regexp-replace #rx"^#lang[^\n]*" text ""))
  (define in (open-input-string body))
  (port-count-lines! in)
  ;; a Scribble `#lang` reads the module body as text
  (define mode (if (regexp-match? #rx"^#lang[^\n]*scribble" text) 'text 'top))
  (with-handlers ([exn:fail? (lambda (e)
                               (format "ERROR: ~a" (car (string-split (exn-message e) "\n"))))])
    (normalize (syntax->datum (parse-all in #:mode mode)))))

(for ([path (in-vector (current-command-line-arguments))])
  (printf "~a\t~a\n" path (parse-file path)))
