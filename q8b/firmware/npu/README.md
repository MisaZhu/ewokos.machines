# SC8280XP NSP0 firmware

`qccdsp8280.mbn` is the signed SC8280XP NSP0/cDSP image used by the Radxa
SC8280XP firmware package.  It is installed by `make basic` at both paths used
by the Q8B bring-up code:

```text
/etc/firmware/qccdsp8280.mbn
/etc/firmware/qcom/sc8280xp/qccdsp8280.mbn
/etc/firmware/qcom/sc8280xp/LENOVO/21BX/qccdsp8280.mbn
```

The source blob is the `qcom/sc8280xp/qccdsp8280.mbn` entry from
[`radxa-pkg/radxa-firmware`](https://github.com/radxa-pkg/radxa-firmware),
blob SHA `5d3175437cc68a83e8ba5072c09c70a2efb2729c`.

```text
size: 3571712 bytes
md5:  aa794aee23732765ca1ab8a97c069adc
```

This is only the signed NSP0 image.  It does not include Qualcomm QAIRT/QNN,
`libcdsprpc`, or an HTP skel.  The apps_smmu and FastRPC data path has now been verified with the official
QAIRT V68 Calculator Skel on real hardware. Dynamic unsigned USER_PD requires
the separate Q8B FastRPC shell, plus AP file/memory listener services. External
SDK/runtime staging is described in `../../docs/q8b-qairt-sdk.md`. Calculator
success does not establish HTP graph execution or CNN performance.
