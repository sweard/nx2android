# OCTOPATH TRAVELER 0, Chinese release (01001300232F4000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # Chinese 1.0.4 (v262144): main+0x12e38/0x12e3c forms the allocator object at 0x68022c8; the call at 0x12e40 is bl 0x9f480.
  # Its constructor writes the UE5 vtable at 0x5b06f38 (adrp/add at 0x9f4b4/0x9f4b8), matching the host allocator row in ue_malloc.tsv.
  'c7c4e3fe091eb9a0adc4df5cbe0c9da9': [
    (0x12e40, '90310294', '1f2003d5', 'nninitStartup: bl 0x9f480 (FMallocBinned3 constructor, 37 GB of address space) → nop; the host allocator is in the object already (hle_uemalloc.c)'),
  ],
  # Octopath Traveler 0 1.0.5 (Chinese release, v327680) main: Unreal's nninitStartup (main+0x12a50) constructs the engine allocator, an FMallocBinned3, in place at main+0x68052c8 (main+0x12e40 → constructor
  # main+0x9f480). Its constructor reserves 37 GB of address space (74 pools of 512 MB, pool index = offset >> 29), which only the Mac can give: an iPhone has about 5 GB without the
  # extended virtual addressing entitlement, and the pools are too uneven to shrink (at character creation the largest holds 68 MB, the 74 together 732 MB). The constructor is not called;
  # the host puts its own allocator's vtable in the object instead (dm/host/hle_uemalloc.c: Malloc, Realloc, Free and the size queries on a host malloc zone), and every allocation goes through it
  # (FMemory reaches the object through FPlatformMemory::BaseAllocator, main+0x132b4, and the thread-local storage allocator nninitStartup registers, main+0x12e80 / main+0x12ec0, through its vtable)
  # The Chinese release's main is the international one's (9fe60a212cff9b9a3656ab63d4f882e5) with the engine's data 0x3000 bytes higher: the cut site and the constructor are at the same addresses and the
  # same bytes, the object is at main+0x68052c8 (the international one's main+0x68022c8) and its vtable at main+0x5b09f38 (main+0x5b06f38)
  'e3e401c28d9fd3b1f9e033bb7bb31d42': [
    (0x12e40, '90310294', '1f2003d5', 'nninitStartup: bl 0x9f480 (FMallocBinned3 constructor, 37 GB of address space) → nop; the host allocator is in the object already (hle_uemalloc.c)'),
  ],
}
