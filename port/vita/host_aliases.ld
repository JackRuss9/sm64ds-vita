/* Hand-checked aliases for the full-game GCC link, appended to the generated
 * romimage_alias.ld. Each names a method the source declares but the ROM never
 * defined under that name, and the symbol whose code the ROM's vtable slot
 * actually holds.
 *
 * daDonketu_c::Render / ::CleanupResources: daDonketu_c redeclares them, but
 * its vtable slots 9 and 3 hold daOts_c's (notes/rtti-reconciliation.md
 * sections 8-9: the config names were misattributed to the child). */
PROVIDE(_ZN11daDonketu_c6RenderEv = _ZN7daOts_c6RenderEv);
PROVIDE(_ZN11daDonketu_c16CleanupResourcesEv = _ZN7daOts_c16CleanupResourcesEv);
