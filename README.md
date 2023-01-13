# NGC history sync prototype/PoC

very experimental and not for production

it uses just the peer_key and pseudo message id to gossip and request using [filetransfers](https://github.com/Green-Sky/tox_ngc_ft1)

uses [tox_ngc_ext](https://github.com/Green-Sky/tox_ngc_ext) for custom packets (gossip)

`ngc_hs1.h` is the public c interface

the .hpp is private

