// Linker scripts alias the TV's exports to these build-only placeholders.
void dummy(void) {
    __builtin_trap();
}

void* dummy_data;
