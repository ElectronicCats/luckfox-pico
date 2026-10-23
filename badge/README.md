# Badge Hack GDL 2026 — capa de firmware

Capa de build del firmware de la badge sobre `luckfox-pico` (RV1106, SPI NAND,
BusyBox, cámara SC3336). La documentación de usuario, hardware, MTP, editor y
rutinas RGB está centralizada en el repositorio principal:

**https://github.com/ElectronicCats/badge_hackgdl_2026**

Este README cubre únicamente la compilación y el flasheo de la capa.

## Contenido

```text
badge/
├── app/            badge_photo, overlay_web, rndis_dhcp y uMTP-Responder
├── configs/        board config, overlay de rootfs y post-script del SDK
├── dts/            device tree de la badge
├── kernel/         fragmento de kernel (cámara, spidev, evdev, USB)
├── mcu/            board RT-Thread del HPMCU (SC3336)
├── meta/           init del sensor y tablas AE/AWB
├── scripts/        build, preparación del SDK, flasheo y pruebas
├── tests/          pruebas host de la app y de la configuración MTP
├── third_party/    uMTP-Responder (GPL-3.0)
└── build.sh        entrada rápida de build
```

## Requisitos

- Linux x86_64 (probado en Ubuntu).
- Git, make, gcc/g++, bc, cpio, rsync, fakeroot, dtc, mkimage, python3, scons.
- ~25 GB libres para el SDK y los artefactos.
- `upgrade_tool` del SDK para flashear.
- Opcional: `adb` para diagnóstico y pruebas de hardware.

El SDK incluye el toolchain ARM. El toolchain RISC-V del HPMCU (~550 MB) se
descarga automáticamente en la primera compilación. Si faltan `bison`, `flex`,
`m4` o `python`, instálalos sin sudo:

```bash
./badge/scripts/install-host-tools.sh
```

## Compilar

Desde la raíz de `luckfox-pico`, en la rama `badge_hackgdl_2026_v2.0.0`:

```bash
./badge/build.sh check     # herramientas del host
./badge/build.sh prepare   # crea build/luckfox-pico y aplica la capa
./badge/build.sh all       # build completo desde fuentes
```

La primera vez crea un worktree del SDK en `badge/build/luckfox-pico` a partir
de `main` y copia esta capa dentro. Las compilaciones siguientes reutilizan el
worktree.

Etapas disponibles: `media`, `uboot`, `kernel`, `rootfs`, `app`, `pack`,
`firmware`, `clean`.

Resultado:

```text
badge/out/update.img
```

El empaquetador de Rockchip incluye la hora de compilación, por lo que un
rebuild limpio no es byte-idéntico a la imagen de release. La imagen validada y
su SHA256 están documentados en el README principal y se publicarán como
artefacto de GitHub Releases; el binario no se versiona en este repositorio.

## Flashear

1. Conecta la badge por USB-C.
2. Entra a **maskrom**: mantén **BOOT**, pulsa y suelta **RST**, sigue
   sosteniendo **BOOT** ~5 s hasta que el host detecte `2207:110c` o
   `2207:350a`, y suelta **BOOT**.
3. Flashea:

```bash
sudo ./badge/scripts/flash.sh badge/out/update.img
```

O con `upgrade_tool`:

```bash
sudo badge/build/luckfox-pico/tools/linux/Linux_Upgrade_Tool/upgrade_tool \
    UF badge/out/update.img
```

> Usa siempre `UF`; `DI -b` no es fiable en esta SPI NAND.

Si la badge ya está en Linux con ADB, `./badge/scripts/maskrom.sh` la reinicia
en maskrom.

## Pruebas

```bash
./badge/scripts/test.sh                  # tests host
./badge/scripts/test-mtp.sh              # prueba MTP desde el host
./badge/scripts/test-hardware.sh --count 2 --reboot
```

## Documentación

La documentación de usuario (alimentación, botones, fotos, MTP, editor web,
rutinas RGB y troubleshooting) está en el repositorio principal para evitar
duplicarla:

- Primeros pasos y uso: https://github.com/ElectronicCats/badge_hackgdl_2026
- Build y flasheo: este README
