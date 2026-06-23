/* Vendored C in a dependency spice. Linked into any consumer that depends on
 * aux-c-vendor, exercising cross-spice :c-sources propagation. */
int vendor_value(void) {
    return 7;
}
