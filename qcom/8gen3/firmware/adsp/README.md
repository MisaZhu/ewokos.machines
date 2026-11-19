# SC8280XP ADSP firmware (TW220 / charger_pd + battmgr)

`qcadsp8280.mbn` is the signed SC8280XP ADSP (LPASS) image from the Robo & Kala
TW220 Windows `tw220-subsystem-update` package.  It is the OEM image that
brings up the ADSP `charger_pd` service, which is the only real battery
manager on this board (the AP-side fuel-gauge registers are dormant without
it).  Installed by `make basic` at:

```text
/etc/firmware/qcadsp8280.mbn
```

The companion service descriptors are staged under `/etc/firmware/adsp/` for
the Phase 2/3 GLINK/GPR transport:

```text
RADS.bin       ADSP resource metadata
adspr.jsn      AudioReach SPF service graph
adspua.jsn     user-accessible service graph
battmgr.jsn    charger_pd battery-manager config (qmi instance 74)
cdspr.jsn      cDSP service graph
```

Source blob: `firmware/qcom/sc8280xp/Ntmer/tw220/qcadsp8280.mbn` from the
TW220 Windows DriverStore `tw220-subsystem-update-20261116` package.

```text
size:   14549356 bytes
sha256: 4532ea116b8185c53f3036e5a5486122a711a6ff3bdc321ef021440b68796549
```

Do not use the Lenovo X13s `qcadsp8280.mbn` (14,565,740 bytes): it is a
different build and does not match the TW220 battery/charger configuration.
