#!/usr/bin/env python3
# Generate a native CIR stack-continuation regression without a large fixture.

DEPTH = 1600


def pointer():
    print('    %pointer = cir.alloca "pointer" align(8) : !pp')


def initialize(name):
    pointer()
    print(f'    %null = cir.const #cir.ptr<null> : !p loc("parallel-{name}":1:1)')
    print('    cir.store %null, %pointer : !p, !pp')


def read(name):
    print(f'    %value = cir.load deref %pointer : !pp, !p loc("parallel-{name}":2:1)')


def simple(name):
    print(f'  cir.func @{name}() {{')
    initialize(name)
    read(name)
    print('    cir.return\n  }')


print('!i = !cir.int<s, 32>\n!p = !cir.ptr<!i>\n!pp = !cir.ptr<!p>\nmodule {')
simple('before')
print('  cir.func @deep(%condition: !cir.bool) {')
initialize('deep')
for _ in range(DEPTH):
    print('    cir.scope {')
read('deep')
print('    cir.brcond %condition ^left, ^right loc("parallel-deep":3:1)')
print('  ^left:\n    cir.yield\n  ^right:\n    cir.yield')
for index in range(DEPTH):
    print('    }')
    if index + 1 != DEPTH:
        print('    cir.yield')
print('    cir.return\n  }')
simple('after')
print('}')
