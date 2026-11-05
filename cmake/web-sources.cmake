# Pinned npm tarballs for the web UI (Leaflet, ECharts): URL, SHA-256 and the
# file name expected in SKYCUE_SOURCE_CACHE (default: sources/ beside the top
# CMakeLists.txt). cmake/VendoredWeb.cmake reads these declarations.

skycue_declare_source(leaflet URL "https://registry.npmjs.org/leaflet/-/leaflet-1.9.4.tgz" SHA256 84c65a256e50657896f54c33bd857b6849ebe94c817803be818bf32a3dde0b77 FILE "leaflet-1.9.4.tgz")
skycue_declare_source(echarts URL "https://registry.npmjs.org/echarts/-/echarts-5.6.0.tgz" SHA256 e1b82487dd7f8d9f0438eec1f1433051148e0095ac7a15d4db890afda45cf610 FILE "echarts-5.6.0.tgz")
