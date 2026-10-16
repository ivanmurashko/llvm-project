// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{}' --dump-config -- | FileCheck %s --check-prefix=CHECK-DEFAULTS
// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{CheckOptions: {cir-lifetime-check.HistoryList: "invalid;null", cir-lifetime-check.RemarksList: "pset-invalid;pset-always", cir-lifetime-check.HistLimit: 2}}' --dump-config -- | FileCheck %s --check-prefix=CHECK-CONFIG
// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{CheckOptions: {cir-lifetime-check.HistoryList: "", cir-lifetime-check.RemarksList: "all"}}' --export-fixes=%t.yaml -- -target x86_64-unknown-linux-gnu 2>&1 | FileCheck %s --check-prefix=CHECK-REMARKS --implicit-check-not='note:'
// RUN: FileCheck %s --input-file=%t.yaml --check-prefix=CHECK-YAML
// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{CheckOptions: {cir-lifetime-check.HistLimit: 0}}' -- -target x86_64-unknown-linux-gnu 2>&1 | FileCheck %s --check-prefix=CHECK-NO-HISTORY --implicit-check-not='note:' --implicit-check-not='remark:'
// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{}' -- -target x86_64-unknown-linux-gnu -DCOMPILE_OPTIONS -O0 2>&1 | count 0
// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{}' -- -target x86_64-unknown-linux-gnu -DCOMPILE_OPTIONS -O1 2>&1 | FileCheck %s --check-prefix=CHECK-OPTIMIZED
// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{}' -- -target x86_64-unknown-linux-gnu -DUNMAPPED_LOCATION 2>&1 | FileCheck %s --check-prefix=CHECK-UNMAPPED
// RUN: not clang-tidy %s --checks='-*,cir-lifetime-check' --config='{}' --export-fixes=%t.error.yaml -- -target x86_64-unknown-linux-gnu -DCODEGEN_ERROR 2>&1 | FileCheck %s --check-prefix=CHECK-ERROR
// RUN: FileCheck %s --input-file=%t.error.yaml --check-prefix=CHECK-ERROR-YAML
// RUN: not clang-tidy %s %S/lifetime-basic.cpp --checks='-*,cir-lifetime-check' --config='{}' --export-fixes=%t.multiple.yaml -- -target x86_64-unknown-linux-gnu -DCODEGEN_ERROR 2>&1 | FileCheck %s --check-prefix=CHECK-MULTIPLE
// RUN: FileCheck %s --input-file=%t.multiple.yaml --check-prefix=CHECK-MULTIPLE-YAML
// RUN: not clang-tidy %s --checks='-*,cir-lifetime-check,clang-diagnostic-sync-alignment' --config='{}' --export-fixes=%t.pragma.yaml -- -target x86_64-unknown-linux-gnu -DPRAGMA_DIAGNOSTICS -Wno-address-of-packed-member 2>&1 | FileCheck %s --check-prefix=CHECK-PRAGMA
// RUN: FileCheck %s --input-file=%t.pragma.yaml --check-prefix=CHECK-PRAGMA-YAML
// RUN: rm -rf %t.profiles %t.missing
// RUN: mkdir %t.profiles
// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{CheckOptions: {cir-lifetime-check.NumThreads: 1, cir-lifetime-check.ProfileOutput: "%t.profiles/cir.json"}}' --enable-check-profile --store-check-profile=%t.profiles/check --export-fixes=%t.profiles/serial.yaml -- -target x86_64-unknown-linux-gnu 2>&1 | FileCheck %s --check-prefix=CHECK-NO-HISTORY
// RUN: FileCheck %s --input-file=%t.profiles/cir.json --check-prefix=CHECK-PROFILE
// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{CheckOptions: {cir-lifetime-check.NumThreads: 2}}' --export-fixes=%t.profiles/threaded.yaml -- -target x86_64-unknown-linux-gnu 2>&1 | FileCheck %s --check-prefix=CHECK-NO-HISTORY
// RUN: diff %t.profiles/serial.yaml %t.profiles/threaded.yaml
// RUN: %python -c "import glob,json; paths=glob.glob(r'%t.profiles/check/*.json'); assert len(paths)==1; p=json.load(open(paths[0])); assert all('time.clang-tidy.cir-lifetime-check.'+k in p['profile'] for k in ('wall','user','sys'))"
// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{CheckOptions: {cir-lifetime-check.ProfileOutput: "%t.missing/profile.json"}}' -- -target x86_64-unknown-linux-gnu 2>&1 | FileCheck %s --check-prefix=CHECK-PROFILE-ERROR
// RUN: not clang-tidy %s --checks='-*,cir-lifetime-check' --config='{CheckOptions: {cir-lifetime-check.ProfileOutput: "%t.profiles/incomplete.json"}}' -- -target x86_64-unknown-linux-gnu -DCODEGEN_ERROR 2>&1 | FileCheck %s --check-prefix=CHECK-ERROR
// RUN: FileCheck %s --input-file=%t.profiles/incomplete.json --check-prefix=CHECK-PARTIAL-PROFILE
// RUN: clang-tidy %s --checks='-*,cir-lifetime-check' --config='{CheckOptions: {cir-lifetime-check.NumThreads: -1}}' -- -target x86_64-unknown-linux-gnu 2>&1 | FileCheck %s --check-prefix=CHECK-INVALID-THREADS

// A CIR generation error in the first translation unit must not prevent the
// second translation unit from being analyzed, including its warning and note.
// CHECK-MULTIPLE-DAG: error: ClangIR code gen Not Yet Implemented: __builtin_elementwise_clzg [clang-diagnostic-error]
// CHECK-MULTIPLE-DAG: warning: use of invalid pointer 'p' [cir-lifetime-check]
// CHECK-MULTIPLE-DAG: note: pointee 'x' invalidated at end of scope
// CHECK-MULTIPLE-YAML-DAG: DiagnosticName:  clang-diagnostic-error
// CHECK-MULTIPLE-YAML-DAG: DiagnosticName:  cir-lifetime-check

// CHECK-DEFAULTS: cir-lifetime-check.HistLimit: '1'
// CHECK-DEFAULTS-NEXT: cir-lifetime-check.HistoryList: all
// CHECK-DEFAULTS-NEXT: cir-lifetime-check.NumThreads: '0'
// CHECK-DEFAULTS-NEXT: cir-lifetime-check.ProfileOutput: ''
// CHECK-DEFAULTS-NEXT: cir-lifetime-check.RemarksList: ''
// CHECK-CONFIG: cir-lifetime-check.HistLimit: '2'
// CHECK-CONFIG-NEXT: cir-lifetime-check.HistoryList: {{'?}}invalid;null{{'?}}
// CHECK-CONFIG-NEXT: cir-lifetime-check.NumThreads: '0'
// CHECK-CONFIG-NEXT: cir-lifetime-check.ProfileOutput: ''
// CHECK-CONFIG-NEXT: cir-lifetime-check.RemarksList: {{'?}}pset-invalid;pset-always{{'?}}

// CHECK-PROFILE: "complete": true
// CHECK-PROFILE: "requested_threads": 1
// CHECK-PROFILE: "effective_threads": 1
// CHECK-PROFILE-DAG: "name": "CIR generation"
// CHECK-PROFILE-DAG: "name": "CIR verification"
// CHECK-PROFILE-DAG: "name": "{{.*}}CIRCanonicalize{{.*}}"
// CHECK-PROFILE-DAG: "name": "{{.*}}LifetimeCheck{{.*}}"
// CHECK-PARTIAL-PROFILE: "complete": false
// CHECK-PROFILE-ERROR: Unable to write CIR profile
// CHECK-PROFILE-ERROR: warning: use of invalid pointer 'p' [cir-lifetime-check]
// CHECK-INVALID-THREADS: warning: invalid configuration value '-1' for option 'cir-lifetime-check.NumThreads'
// CHECK-INVALID-THREADS: warning: use of invalid pointer 'p' [cir-lifetime-check]

#ifdef CODEGEN_ERROR
void unsupported(unsigned value) {
  (void)__builtin_elementwise_clzg(value);
}
// CHECK-ERROR: error: ClangIR code gen Not Yet Implemented: __builtin_elementwise_clzg [clang-diagnostic-error]
// CHECK-ERROR-YAML: DiagnosticName:  clang-diagnostic-error
// CHECK-ERROR-YAML: Message:{{.*}}ClangIR code gen Not Yet Implemented: __builtin_elementwise_clzg
// CHECK-ERROR-YAML: Level:           Error
#elif defined(PRAGMA_DIAGNOSTICS)
struct __attribute__((packed)) Packed {
  char padding;
  int value;
};
int source_diagnostic_state(Packed *p) {
  int result = __sync_fetch_and_add(&p->value, 1);
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wsync-alignment"
  result += __sync_fetch_and_add(&p->value, 1);
#pragma clang diagnostic pop
  return result + __sync_fetch_and_add(&p->value, 1);
}
// CHECK-PRAGMA: warning: {{.*}} [clang-diagnostic-sync-alignment]
// CHECK-PRAGMA: error: {{.*}} [clang-diagnostic-sync-alignment]
// CHECK-PRAGMA: warning: {{.*}} [clang-diagnostic-sync-alignment]
// CHECK-PRAGMA-YAML: DiagnosticName:  clang-diagnostic-sync-alignment
// CHECK-PRAGMA-YAML: Level:           Warning
// CHECK-PRAGMA-YAML: DiagnosticName:  clang-diagnostic-sync-alignment
// CHECK-PRAGMA-YAML: Level:           Error
// CHECK-PRAGMA-YAML: DiagnosticName:  clang-diagnostic-sync-alignment
// CHECK-PRAGMA-YAML: Level:           Warning
#elif defined(COMPILE_OPTIONS)
// CIR only evaluates __builtin_expect's expected-value argument when optimizing.
// This warning therefore depends on the active CompilerInstance's CodeGenOptions
// reaching CIRGenerator, rather than default-constructed CodeGenOptions.
long compile_options(long value) {
  long *p = nullptr;
  return __builtin_expect(value, *p);
}
// CHECK-OPTIMIZED: warning: use of invalid pointer 'p' [cir-lifetime-check]
// CHECK-OPTIMIZED: note: 'nullptr' invalidated here
#elif defined(UNMAPPED_LOCATION)
#line 200 "cir-virtual-source.cpp"
void virtual_source() {
  int *p = nullptr;
  *p = 42;
}
// CHECK-UNMAPPED: warning: cir-virtual-source.cpp:202:{{[0-9]+}}: use of invalid pointer 'p' [cir-lifetime-check]
// CHECK-UNMAPPED: note: cir-virtual-source.cpp:201:{{[0-9]+}}: 'nullptr' invalidated here
#else
void dangling() {
  int *p;
  {
    int x = 0;
    p = &x;
    *p = 42;
  }
  *p = 42;
}
// CHECK-REMARKS: remark: pset => { x } [cir-lifetime-check]
// CHECK-REMARKS: remark: pset => { invalid } [cir-lifetime-check]
// CHECK-REMARKS: warning: use of invalid pointer 'p' [cir-lifetime-check]
// CHECK-NO-HISTORY: warning: use of invalid pointer 'p' [cir-lifetime-check]

// CHECK-YAML: DiagnosticName:  cir-lifetime-check
// CHECK-YAML: Message:         'pset => { x }'
// CHECK-YAML: Level:           Remark
// CHECK-YAML: DiagnosticName:  cir-lifetime-check
// CHECK-YAML: Message:         'pset => { invalid }'
// CHECK-YAML: Level:           Remark
// CHECK-YAML: DiagnosticName:  cir-lifetime-check
// CHECK-YAML: Message:         'use of invalid pointer ''p'''
// CHECK-YAML: Level:           Warning
#endif
