;;;; a bunch of handy macros for x86-64

;;;; This software is part of the SBCL system. See the README file for
;;;; more information.
;;;;
;;;; This software is derived from the CMU CL system, which was
;;;; written at Carnegie Mellon University and released into the
;;;; public domain. The software is in the public domain and is
;;;; provided with absolutely no warranty. See the COPYING and CREDITS
;;;; files for more information.

(in-package "SB!VM")

;;;; instruction-like macros

;;; This used to be a macro (and still is on the other platforms) but
;;; the support for SC-dependent move instructions needed here makes
;;; that expand into so large an expression that the resulting code
;;; bloat is not justifiable.
(defun move (dst src)
  "Move SRC into DST unless they are location=."
  (unless (location= dst src)
    (sc-case dst
      ((single-reg complex-single-reg)
       (aver (xmm-register-p src))
       (inst movaps dst src))
      ((double-reg complex-double-reg)
       (aver (xmm-register-p src))
       (inst movapd dst src))
      #!+sb-simd-pack
      ((int-sse-reg sse-reg)
       (aver (xmm-register-p src))
       (inst movdqa dst src))
      #!+sb-simd-pack
      ((single-sse-reg double-sse-reg)
       (aver (xmm-register-p src))
       (inst movaps dst src))
      (t
       (inst mov dst src)))))

(defmacro make-ea-for-object-slot (ptr slot lowtag)
  `(make-ea :qword :base ,ptr :disp (- (* ,slot n-word-bytes) ,lowtag)))
(defmacro make-ea-for-object-slot-half (ptr slot lowtag)
  `(make-ea :dword :base ,ptr :disp (- (* ,slot n-word-bytes) ,lowtag)))
(defmacro tls-index-of (sym)
  `(make-ea :dword :base ,sym :disp (+ 4 (- other-pointer-lowtag))))

(defmacro loadw (value ptr &optional (slot 0) (lowtag 0))
  `(inst mov ,value (make-ea-for-object-slot ,ptr ,slot ,lowtag)))

(defun storew (value ptr &optional (slot 0) (lowtag 0))
  (cond ((and (integerp value)
              (not (typep value '(signed-byte 32))))
         (inst mov temp-reg-tn value)
         (inst mov (make-ea-for-object-slot ptr slot lowtag) temp-reg-tn))
        (t
         (inst mov (make-ea-for-object-slot ptr slot lowtag) value))))

(defmacro pushw (ptr &optional (slot 0) (lowtag 0))
  `(inst push (make-ea-for-object-slot ,ptr ,slot ,lowtag)))

(defmacro popw (ptr &optional (slot 0) (lowtag 0))
  `(inst pop (make-ea-for-object-slot ,ptr ,slot ,lowtag)))


;;;; macros to generate useful values

(defmacro load-symbol (reg symbol)
  `(inst mov ,reg (+ nil-value (static-symbol-offset ,symbol))))

(defmacro make-ea-for-symbol-value (symbol)
  `(make-ea :qword
    :disp (+ nil-value
           (static-symbol-offset ',symbol)
           (ash symbol-value-slot word-shift)
           (- other-pointer-lowtag))))

(defmacro load-symbol-value (reg symbol)
  `(inst mov ,reg (make-ea-for-symbol-value ,symbol)))

(defmacro store-symbol-value (reg symbol)
  `(inst mov (make-ea-for-symbol-value ,symbol) ,reg))

;; Return the effective address of the value slot of static SYMBOL.
(defun static-symbol-value-ea (symbol)
   (make-ea :qword
            :disp (+ nil-value
                     (static-symbol-offset symbol)
                     (ash symbol-value-slot word-shift)
                     (- other-pointer-lowtag))))

(defun thread-tls-ea (index &optional (size :qword))
  (if (tn-p index)
      ;; Due to an encoding peculiarity, flipping the base and index is better.
      ;; Base of r13 is reg=5 in ModRegRM, so if mod were 0, it would imply
      ;; RIP-relative addressing. (And attempting to encode an index is illegal)
      ;; So the 'mod' bits must be nonzero, which mandates encoding of an
      ;; explicit displacement of 0.  Using INDEX as base avoids the extra byte.
      (make-ea size :base index :index thread-base-tn)
      (make-ea size :base thread-base-tn :disp index)))

#!+sb-thread
(progn
  ;; Return an EA for the TLS of SYMBOL, or die.
  (defun symbol-known-tls-cell (symbol)
    (let ((index (info :variable :wired-tls symbol)))
      (aver (integerp index))
      (thread-tls-ea index)))

  ;; LOAD/STORE-TL-SYMBOL-VALUE macros are ad-hoc (ugly) emulations
  ;; of (INFO :VARIABLE :WIRED-TLS) = :ALWAYS-THREAD-LOCAL
  (defmacro load-tl-symbol-value (reg symbol)
    `(inst mov ,reg (symbol-known-tls-cell ',symbol)))

  (defmacro store-tl-symbol-value (reg symbol)
    `(inst mov (symbol-known-tls-cell ',symbol) ,reg)))

#!-sb-thread
(progn
  (defmacro load-tl-symbol-value (reg symbol)
    `(load-symbol-value ,reg ,symbol))
  (defmacro store-tl-symbol-value (reg symbol)
    `(store-symbol-value ,reg ,symbol)))

(defmacro load-binding-stack-pointer (reg)
  #!+sb-thread `(inst mov ,reg (symbol-known-tls-cell '*binding-stack-pointer*))
  #!-sb-thread `(load-symbol-value ,reg *binding-stack-pointer*))

(defmacro store-binding-stack-pointer (reg)
  #!+sb-thread `(inst mov (symbol-known-tls-cell '*binding-stack-pointer*) ,reg)
  #!-sb-thread `(store-symbol-value ,reg *binding-stack-pointer*))

(defmacro load-type (target source &optional (offset 0))
  "Loads the type bits of a pointer into target independent of
   byte-ordering issues."
  (once-only ((n-target target)
              (n-source source)
              (n-offset offset))
    (ecase *backend-byte-order*
      (:little-endian
       `(inst movzx ,n-target
              (make-ea :byte :base ,n-source :disp ,n-offset)))
      (:big-endian
       `(inst movzx ,n-target
              (make-ea :byte :base ,n-source
                             :disp (+ ,n-offset (1- n-word-bytes))))))))

;;;; error code
(defun emit-error-break (vop kind code values)
  (assemble ()
    #!-ud2-breakpoints
    (inst int #!+int4-breakpoints 4 #!-int4-breakpoints 3)
    ;; On Darwin, we need to use #x0b0f instead of int3 in order
    ;; to generate a SIGILL instead of a SIGTRAP as darwin/x86
    ;; doesn't seem to be reliably firing SIGTRAP
    ;; handlers. Hopefully this will be fixed by Apple at a
    ;; later date. Do the same on x86-64 as we do on x86 until this gets
    ;; sorted out.
    #!+ud2-breakpoints
    (inst word #x0b0f)
    ;; The return PC points here; note the location for the debugger.
    (when vop
      (note-this-location vop :internal-error))
    (if (= kind invalid-arg-count-trap) ; there is no "payload" in this trap kind
        (inst byte kind)
        (emit-internal-error kind code values))))

(defun generate-error-code (vop error-code &rest values)
  (apply #'generate-error-code+ nil vop error-code values))

(defun generate-error-code+ (preamble-emitter vop error-code &rest values)
  "Generate-Error-Code Error-code Value*
  Emit code for an error with the specified Error-Code and context Values."
  (assemble (*elsewhere*)
    (let ((start-lab (gen-label)))
      (emit-label start-lab)
      (when preamble-emitter
        (funcall preamble-emitter))
      (emit-error-break vop
                        (case error-code ; should be named ERROR-SYMBOL really
                          (invalid-arg-count-error invalid-arg-count-trap)
                          (t error-trap))
                        (error-number-or-lose error-code)
                        values)
      start-lab)))


;;;; PSEUDO-ATOMIC

;;; This is used to wrap operations which leave untagged memory lying
;;; around.  It's an operation which the AOP weenies would describe as
;;; having "cross-cutting concerns", meaning it appears all over the
;;; place and there's no logical single place to attach documentation.
;;; grep (mostly in src/runtime) is your friend

(defmacro maybe-pseudo-atomic (not-really-p &body body)
  `(if ,not-really-p
       (progn ,@body)
       (pseudo-atomic ,@body)))

;;; Unsafely clear pa flags so that the image can properly lose in a
;;; pa section.
#!+sb-thread
(defmacro %clear-pseudo-atomic ()
  '(inst mov (thread-tls-ea (* n-word-bytes thread-pseudo-atomic-bits-slot)) 0))

#!+sb-safepoint
(defun emit-safepoint ()
  (inst test al-tn (make-ea :byte :disp
                            (- nil-value n-word-bytes other-pointer-lowtag
                               gc-safepoint-trap-offset))))

(defmacro pseudo-atomic (&rest forms)
  #!+sb-safepoint-strictly
  `(progn ,@forms (emit-safepoint))
  #!-sb-safepoint-strictly
  (with-unique-names (label pa-bits-ea)
    `(let ((,label (gen-label))
           (,pa-bits-ea
            #!+sb-thread
            (thread-tls-ea (* n-word-bytes thread-pseudo-atomic-bits-slot))
            #!-sb-thread
            (make-ea :qword
                     :disp (+ nil-value
                            (static-symbol-offset
                             '*pseudo-atomic-bits*)
                            (ash symbol-value-slot word-shift)
                            (- other-pointer-lowtag)))))
       (inst mov ,pa-bits-ea rbp-tn)
       ,@forms
       (inst xor ,pa-bits-ea rbp-tn)
       (inst jmp :z ,label)
       ;; if PAI was set, interrupts were disabled at the same time
       ;; using the process signal mask.
       (inst break pending-interrupt-trap)
       (emit-label ,label)
       #!+sb-safepoint
       ;; In this case, when allocation thinks a GC should be done, it
       ;; does not mark PA as interrupted, but schedules a safepoint
       ;; trap instead.  Let's take the opportunity to trigger that
       ;; safepoint right now.
       (emit-safepoint))))

;;;; indexed references

(defmacro define-full-compare-and-swap
    (name type offset lowtag scs el-type &optional translate)
  `(progn
     (define-vop (,name)
         ,@(when translate `((:translate ,translate)))
       (:policy :fast-safe)
       (:args (object :scs (descriptor-reg) :to :eval)
              (index :scs (,@(when (member translate '(%instance-cas %raw-instance-cas/word))
                               '(immediate))
                           any-reg) :to :eval)
              (old-value :scs ,scs :target rax)
              (new-value :scs ,scs))
       (:arg-types ,type tagged-num ,el-type ,el-type)
       (:temporary (:sc descriptor-reg :offset rax-offset
                        :from (:argument 2) :to :result :target value)  rax)
       (:results (value :scs ,scs))
       (:result-types ,el-type)
       (:generator 5
         (move rax old-value)
         (inst cmpxchg
               (make-ea :qword :base object
                        :index  (unless (sc-is index immediate) index)
                        :scale (ash 1 (- word-shift n-fixnum-tag-bits))
                        :disp (- (* (+ (if (sc-is index immediate) (tn-value index) 0)
                                       ,offset) n-word-bytes) ,lowtag))
               new-value :lock)
         (move value rax)))))

(defmacro define-full-reffer (name type offset lowtag scs el-type &optional translate)
  `(progn
     (define-vop (,name)
       ,@(when translate
           `((:translate ,translate)))
       (:policy :fast-safe)
       (:args (object :scs (descriptor-reg))
              (index :scs (any-reg)))
       (:arg-types ,type tagged-num)
       (:results (value :scs ,scs))
       (:result-types ,el-type)
       (:generator 3                    ; pw was 5
         (inst mov value (make-ea :qword :base object :index index
                                  :scale (ash 1 (- word-shift n-fixnum-tag-bits))
                                  :disp (- (* ,offset n-word-bytes)
                                           ,lowtag)))))
     (define-vop (,(symbolicate name "-C"))
       ,@(when translate
           `((:translate ,translate)))
       (:policy :fast-safe)
       (:args (object :scs (descriptor-reg)))
       (:info index)
       (:arg-types ,type
                   (:constant (load/store-index ,n-word-bytes ,(eval lowtag)
                                                ,(eval offset))))
       (:results (value :scs ,scs))
       (:result-types ,el-type)
       (:generator 2                    ; pw was 5
         (inst mov value (make-ea :qword :base object
                                  :disp (- (* (+ ,offset index) n-word-bytes)
                                           ,lowtag)))))))

(defmacro define-full-reffer+offset (name type offset lowtag scs el-type &optional translate)
  `(progn
     (define-vop (,name)
       ,@(when translate
           `((:translate ,translate)))
       (:policy :fast-safe)
       (:args (object :scs (descriptor-reg))
              (index :scs (any-reg)))
       (:info offset)
       (:arg-types ,type tagged-num
                   (:constant (constant-displacement other-pointer-lowtag
                                                     n-word-bytes vector-data-offset)))
       (:results (value :scs ,scs))
       (:result-types ,el-type)
       (:generator 3                    ; pw was 5
         (inst mov value (make-ea :qword :base object :index index
                                  :scale (ash 1 (- word-shift n-fixnum-tag-bits))
                                  :disp (- (* (+ ,offset offset) n-word-bytes)
                                           ,lowtag)))))
     (define-vop (,(symbolicate name "-C"))
       ,@(when translate
           `((:translate ,translate)))
       (:policy :fast-safe)
       (:args (object :scs (descriptor-reg)))
       (:info index offset)
       (:arg-types ,type
                   (:constant (load/store-index ,n-word-bytes ,(eval lowtag)
                                                ,(eval offset)))
                   (:constant (constant-displacement other-pointer-lowtag
                                                     n-word-bytes vector-data-offset)))
       (:results (value :scs ,scs))
       (:result-types ,el-type)
       (:generator 2                    ; pw was 5
         (inst mov value (make-ea :qword :base object
                                  :disp (- (* (+ ,offset index offset) n-word-bytes)
                                           ,lowtag)))))))

(defmacro define-full-setter (name type offset lowtag scs el-type &optional translate)
  `(progn
     (define-vop (,name)
       ,@(when translate
           `((:translate ,translate)))
       (:policy :fast-safe)
       (:args (object :scs (descriptor-reg))
              (index :scs (any-reg))
              (value :scs ,scs :target result))
       (:arg-types ,type tagged-num ,el-type)
       (:results (result :scs ,scs))
       (:result-types ,el-type)
       (:generator 4                    ; was 5
         (gen-cell-set
                   (make-ea :qword :base object :index index
                            :scale (ash 1 (- word-shift n-fixnum-tag-bits))
                            :disp (- (* ,offset n-word-bytes) ,lowtag))
                   value result)))
     (define-vop (,(symbolicate name "-C"))
       ,@(when translate
           `((:translate ,translate)))
       (:policy :fast-safe)
       (:args (object :scs (descriptor-reg))
              (value :scs ,scs :target result))
       (:info index)
       (:arg-types ,type
                   (:constant (load/store-index ,n-word-bytes ,(eval lowtag)
                                                ,(eval offset)))
                   ,el-type)
       (:results (result :scs ,scs))
       (:result-types ,el-type)
       (:generator 3                    ; was 5
         (gen-cell-set
                   (make-ea :qword :base object
                            :disp (- (* (+ ,offset index) n-word-bytes)
                                     ,lowtag))
                   value result)))))

(defmacro define-full-setter+offset (name type offset lowtag scs el-type &optional translate)
  `(progn
     (define-vop (,name)
       ,@(when translate
           `((:translate ,translate)))
       (:policy :fast-safe)
       (:args (object :scs (descriptor-reg))
              (index :scs (any-reg))
              (value :scs ,scs :target result))
       (:info offset)
       (:arg-types ,type tagged-num
                   (:constant (constant-displacement other-pointer-lowtag
                                                     n-word-bytes
                                                     vector-data-offset))
                   ,el-type)
       (:results (result :scs ,scs))
       (:result-types ,el-type)
       (:generator 4                    ; was 5
         (gen-cell-set
                   (make-ea :qword :base object :index index
                            :scale (ash 1 (- word-shift n-fixnum-tag-bits))
                            :disp (- (* (+ ,offset offset) n-word-bytes) ,lowtag))
                   value result)))
     (define-vop (,(symbolicate name "-C"))
       ,@(when translate
           `((:translate ,translate)))
       (:policy :fast-safe)
       (:args (object :scs (descriptor-reg))
              (value :scs ,scs :target result))
       (:info index offset)
       (:arg-types ,type
                   (:constant (load/store-index ,n-word-bytes ,(eval lowtag)
                                                ,(eval offset)))
                   (:constant (constant-displacement other-pointer-lowtag
                                                     n-word-bytes
                                                     vector-data-offset))
                   ,el-type)
       (:results (result :scs ,scs))
       (:result-types ,el-type)
       (:generator 3                    ; was 5
         (gen-cell-set
                   (make-ea :qword :base object
                            :disp (- (* (+ ,offset index offset) n-word-bytes)
                                     ,lowtag))
                   value result)))))

;;; Emit the most compact form of the test immediate instruction
;;; by using the smallest operand size that is the large enough to hold
;;; the immediate value Y. The operand size makes no difference since the result
;;; of the implied AND is not written back to a register. However, if the msb
;;; (the sign bit) of the immediate at a smaller size is 1 but at its true size
;;; (always a :QWORD) is 0, the S flag value could come out 1 instead of 0.
;;; SIGN-BIT-MATTERS specifies that a shorter operand size must not be selected
;;; if doing so could affect whether the sign flag comes out the same.
;;; e.g. if EDX is #xff, "TEST EDX, #x80" indicates a non-negative result
;;; whereas "TEST DL, #x80" indicates a negative result.
(defun emit-optimized-test-inst (x y sign-bit-matters)
  (let* ((size-override
          (cond ((or (typep y '(unsigned-byte 7))
                     (and (typep y '(unsigned-byte 8)) (not sign-bit-matters)))
                 :byte)
                ((or (typep y '(unsigned-byte 15))
                     (and (typep y '(unsigned-byte 16)) (not sign-bit-matters)))
                 :word)
                ((or (typep y '(unsigned-byte 31))
                     (and (typep y '(unsigned-byte 32)) (not sign-bit-matters)))
                 :dword)))
         (offset (tn-offset x))
         (modified-x
          (when size-override
            (cond ((sc-is x control-stack)
                   ;; TODO: a 7- or 8-bit pattern that does not span bytes
                   ;; should be testable as a :BYTE by suitably altering :DISP.
                   (make-ea size-override :base rbp-tn :disp (frame-byte-offset offset)))
                  ((sc-is x any-reg descriptor-reg signed-reg unsigned-reg)
                   (reg-in-size x size-override))))))
    (inst test (or modified-x x) y)))


(defun move-dword-if-immobile-code (dest src)
  (flet ((downsize (tn)
           (cond #!+immobile-code
                 (sb!c::*code-is-immobile*
                  (cond ((sc-is tn signed-stack unsigned-stack sap-stack)
                         (make-ea :dword :base rbp-tn
                                         :disp (frame-byte-offset (tn-offset tn))))
                        (t
                         (reg-in-size tn :dword))))
                 (t
                  tn))))
    (inst mov (downsize dest) (downsize src))))
