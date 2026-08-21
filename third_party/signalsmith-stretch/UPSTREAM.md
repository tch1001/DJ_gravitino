# Signalsmith Stretch vendoring

Gravitino vendors the header-only MIT-licensed Signalsmith Stretch DSP and its
Signalsmith Linear dependency so key lock works without an additional package
manager install.

- `signalsmith-stretch.h`: Signalsmith-Audio/signalsmith-stretch commit
  `57b93f4e9206a089a45387eaa39bdc9f310d3308` (v1.3.2)
- `signalsmith-linear/`: Signalsmith-Audio/linear commit
  `8be69c57b7064822076c2cfc55a522e5f5867cc1`

The corresponding upstream MIT licenses are retained alongside the sources.
