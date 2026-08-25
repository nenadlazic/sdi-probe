# Where the DeckLink SDK goes

The Blackmagic DeckLink SDK is proprietary and is **not** redistributed with this tool.
Download it from <https://www.blackmagicdesign.com/developer/> and unpack the Linux
`include` directory here, named after its version:

```
sdk/
  ACTIVE                        <- names the folder build.sh uses by default
  DeckLink_SDK_12_4_1/
    DeckLinkAPI.h
    DeckLinkAPIDispatch.cpp
    DeckLinkAPIVersion.h
    ...
  DeckLink_SDK_11_5_1/          <- keep as many versions side by side as you like
```

`ACTIVE` holds a single line: the folder name to build against. Change that line to switch
the default version. To build against a different one just this once, without editing
anything:

```bash
SDK_VERSION=11_5_1 ./build.sh     # another folder in here, by suffix
SDK=/opt/decklink-sdk ./build.sh  # an SDK somewhere else entirely
```

The `DeckLink_SDK_*` folders are gitignored, so an unpacked SDK can never be committed
by accident.
