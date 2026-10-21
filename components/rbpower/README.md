# RBPower

Core component of the RBgrid firmware: measurement collection from the rbAmp I²C modules, statistics
and history, the time-of-use tariff/billing engine, storage, the REST/WebSocket API and the MQTT bridge.

The source is in this directory (`src/`, `include/`). RBgrid has no built-in ADC — the rbAmp modules
measure, this component collects, computes and stores. See the root [README](../../README.md) and the
documentation at https://www.rbgrid.io/rbgrid-doc-overview.
