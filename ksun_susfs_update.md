# KSUN 33219 + SuSFS 2.2.0 + KSU_Toolkit — Kernel pstar sm8250 4.19

Modo do gancho: Inline (SuSFS)

Metamodule status: Não instalado

Versão SuSFS: Suportado | v2.2.0 (NON-GKI)

Versão do kernel: 4.19.325-cip134-st18-perf-g89c7b24f7db0 (aarch64)


## Estado Após Atualização (2026-07-26)

### Versões
- KernelSU-Next: **33219** (fallback tag `v3.3.0-legacy`)
- SuSFS: **v2.2.0 (NON-GKI)** — já integrado em `fs/susfs.c` + `include/linux/susfs.h`
- UAPI Version: **2** (`KERNEL_SU_UAPI_VERSION 2`)
- KSU_APP_PROFILE_VER: **4**
- FILE_FORMAT_VERSION (allowlist): **4**

### Causa do Erro Inicial
"Incompatibilidade entre a versão da uapi do gerenciador (2) e a versão da uapi do driver KernelSU (0)"
- `struct ksu_get_info_cmd` não tinha campo `uapi_version`
- `KERNEL_SU_UAPI_VERSION` não estava definido
- `dispatch.c` nunca setava `cmd.uapi_version`
- Kbuild travado em `KSU_MIN_COMPAT_VERSION := 33201`

### Arquivos Modificados
| Arquivo | Mudança |
|---|---|
| `KernelSU-Next/kernel/include/uapi/supercall.h` | Adicionado `KERNEL_SU_UAPI_VERSION 2`, `uapi_version` em `ksu_get_info_cmd`, `ksu_get_info_legacy_cmd`, `KSU_IOCTL_GET_INFO_LEGACY`, `KSU_IOCTL_GET_SULOG_FD`, `KSU_IOCTL_DISABLE_ESCAPE_TO_ROOT`; convertido `static const` → `#define` |
| `KernelSU-Next/uapi/supercall.h` | Idem (manager-facing, mantido em sync) |
| `KernelSU-Next/kernel/include/uapi/feature.h` | Adicionado `KSU_FEATURE_SULOG=2`, `ADB_ROOT=3`, `SELINUX_HIDE_STATUS=4` |
| `KernelSU-Next/kernel/include/uapi/app_profile.h` | `KSU_APP_PROFILE_VER 3→4`, adicionado `FLAG_KSU_NO_NEW_PRIVS`, `__u64 flags` em `root_profile` |
| `KernelSU-Next/kernel/include/uapi/selinux.h` | Convertido `static const` → `#define` |
| `KernelSU-Next/kernel/include/uapi/ksu.h` | Arquivo novo — umbrella include |
| `KernelSU-Next/kernel/include/uapi/sulog.h` | Arquivo novo — sulog UAPI |
| `KernelSU-Next/kernel/supercall/dispatch.c` | Adicionado `cmd.uapi_version = KERNEL_SU_UAPI_VERSION`, função `do_get_info_legacy()`, handler `KSU_IOCTL_GET_INFO_LEGACY` na tabela |
| `KernelSU-Next/kernel/policy/app_profile.h` | Adicionado `#define TIF_KSU_DISABLE_ESCAPE_WITH_ROOT 63` |
| `KernelSU-Next/kernel/policy/app_profile.c` | Verificação de `TIF_KSU_DISABLE_ESCAPE_WITH_ROOT`, set de `TIF_` quando `FLAG_KSU_NO_NEW_PRIVS` |
| `KernelSU-Next/kernel/policy/allowlist.c` | `FILE_FORMAT_VERSION 3→4`, `profile_valid` usa `!=` em vez de `<`, removido código de migração inline, adicionado `migrate_profile()`, `ksu_load_allow_list` com `kAppProfileSizePreV4=776`, auto-persist quando versão antiga |
| `KernelSU-Next/kernel/Kbuild` | `KSU_MIN_COMPAT_VERSION 33201→33219`, tag fallback `v3.2.0-legacy→v3.3.0-legacy` |

### Pasta exemplo/
```
exemplo/ksun/                    — KSUN base 33219 (legacy branch, 2992 commits)
  drivers/kernelsu/              — Driver kernel (= KernelSU-Next/kernel/)
  KernelSU-Next/uapi/            — UAPI headers para manager

exemplo/kernel_patches-main/
  next/susfs_fix_patches/v2.2.0/ — Patches SuSFS 2.2.0 para KSUN (fix_Kbuild, fix_init, etc.)
  next/next_hooks.patch           — Hooks para kernel ≥4.19 (inline, não kprobes)
  next/next_hooks_4.14.patch      — Hooks para kernel ≤4.14

exemplo/susfs4ksu-gki-android16-6.12/
  kernel_patches/                — Patches susfs para GKI android16 6.12
  ksu_module_susfs/              — Módulo KSU susfs
```

### Lógica de Versão KSUN no Kbuild
- Fórmula nova (exemplo): `30000 + git_commits + 200`
- Fórmula atual (sm8250): `30000 + git_commits + 150` com floor de 33219
- Como `sm8250/KernelSU-Next` NÃO é git separado (mesmo root do kernel), git_count NÃO é usado → usa sempre fallback = 33219

### Hook Mode
- Kernel 4.19 usa **inline hooks** (não kprobes)
- Hooks aplicados em `fs/exec.c`, `fs/open.c`, `fs/stat.c` com `#ifdef CONFIG_KSU`
- Hook mode reportado: "Inline (SuSFS)"

### KSU_Toolkit (presente em supercall/supercall.c)
- `CHANGE_MANAGER_UID = 10006` — altera manager UID
- `CHANGE_KSUVER = 10011` — override da versão reportada
- `CHANGE_SPOOF_UNAME = 10012` — spoof de uname
- `GET_SULOG_DUMP_V2 = 10010` — dump de sulog
- `ksuver_override` — variável global que sobrescreve KERNEL_SU_VERSION

### SuSFS 2.2.0 Features (já integradas no kernel)
- `CONFIG_KSU_SUSFS` — enable susfs
- `CONFIG_KSU_SUSFS_SUS_PATH` — esconder paths suspeitos
- `CONFIG_KSU_SUSFS_SUS_MOUNT` — esconder mounts
- `CONFIG_KSU_SUSFS_SUS_KSTAT` — falsificar kstat
- `CONFIG_KSU_SUSFS_TRY_UMOUNT` — auto-umount em processos não-root
- `susfs_set_current_proc_umounted()` — marca processo como não-montado
- SUSFS_VARIANT: "NON-GKI" (kernel < 5.0)

### UAPI Ioctl GET_INFO — Dualidade
```c
// Nova (manager compilado contra UAPI 2):
KSU_IOCTL_GET_INFO = _IOR('K', 2, struct ksu_get_info_cmd)  // inclui tamanho da struct
// Legacy (manager antigo):
KSU_IOCTL_GET_INFO_LEGACY = _IOC(_IOC_READ, 'K', 2, 0)      // tamanho=0
```
Ambos registrados em `dispatch.c` com handlers diferentes.

### app_profile Migração v3→v4
- `root_profile` ganhou `__u64 flags` → struct maior
- `kAppProfileSizePreV4 = 776` bytes (tamanho antigo)
- `migrate_profile()` converte v2/v3 → v4: seta `FLAG_KSU_NO_NEW_PRIVS` para v3
- `ksu_load_allow_list()` auto-detecta versão e persiste após migração
