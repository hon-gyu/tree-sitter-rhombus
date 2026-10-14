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

(define (parse-file path)
  (define text (call-with-input-file path port->string))
  ;; blank out a `#lang` line so positions stay the same
  (define body (regexp-replace #rx"^#lang[^\n]*" text ""))
  (define in (open-input-string body))
  (port-count-lines! in)
  (with-handlers ([exn:fail? (lambda (e)
                               (format "ERROR: ~a" (car (string-split (exn-message e) "\n"))))])
    (normalize (syntax->datum (parse-all in)))))

(for ([path (in-vector (current-command-line-arguments))])
  (printf "~a\t~a\n" path (parse-file path)))
