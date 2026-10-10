# KSUN 33333 + SuSFS 2.3.0 + KSU_Toolkit — Kernel pstar sm8250 4.19

Modo do gancho: Inline (Manual / SuSFS)

Versão SuSFS: Suportado | v2.3.0 (NON-GKI)

Versão do kernel: 4.19.325-cip134-st18-perf-g89c7b24f7db0 (aarch64)

## Estado Atual (2026-10-10)

### Versões
- KernelSU-Next: **33333** (`v3.4.1-legacy`)
- SuSFS: **v2.3.0 (NON-GKI)** — integrado em `fs/susfs.c` + `include/linux/susfs.h`
- UAPI Version: **4** (`KERNEL_SU_UAPI_VERSION 4` - compatível com Manager v3.4.1)
- KSU_APP_PROFILE_VER: **4**
- FILE_FORMAT_VERSION (allowlist): **4**
- Manager Signature: Cert size `0x3e6`, sha256 `79e590113c4c4c0c222978e413a5faa801666957b1212a328e46c00c69821bf7` (pacote `yhaxhr.birgvn.bmwbne`, UID 10401)

### Causa Diagnosticada da Falha em Conceder Root (Logs ADB)
1. **Rejeição do descritor de arquivo do driver para o Manager em `ksu_handle_sys_reboot()` (`supercall.c`)**:
   - Quando o Manager (`yhaxhr.birgvn.bmwbne`, UID 10401) chamava `libksud.so install`, o `ksud` executava como UID 10401 e invocava `reboot(KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, &fd)`.
   - O kernel verificava `if (current_uid().val != 0) return 0;`, recusando a concessão do FD do driver para o Manager!
   - Isso gerava no logcat: `E KernelSU Next: ksud::cli: Error: ksuctl failed: could not retrieve kernelsu driver fd` e o diretório `/data/adb` não era criado/inicializado.
   - Como o `ksud` falhava na instalação, o Manager caía em fallback via `RootService` com jar em cache (`SecurityException: Writable dex file '/data/user_de/0/.../cache/main.jar' is not allowed` no Android 14+ ART).
   - **Correção**: Ajustado `ksu_handle_sys_reboot` para autorizar `uid == 0`, `is_manager()` e UIDs permitidos (`ksu_is_allow_uid_for_current()`), retornando o FD sincronicamente via `copy_to_user`.

2. **Desmontagem indevida e flag umounted em `setuid_hook.c`**:
   - `ksu_handle_setresuid()` caía incondicionalmente em `ksu_handle_umount()` e `susfs_set_current_proc_umounted()` para qualquer app, inclusive apps com concessão root ativa (`ksu_is_allow_uid_for_current(new_uid)`).
   - **Correção**: Alinhado com o patch oficial do SuSFS 2.2/2.3 (`fix_setuid_hook.c.patch`). Apps permitidos recebem bypass do seccomp e retornam 0 sem sofrer umount nem marcação de proc umounted no SuSFS.

3. **Falha de root em aplicativos (`su` -> `sh`)**:
   - Em `sucompat.c`, havia uma checagem preliminar `ksu_filp_open_compat(KSUD_PATH)` que tentava abrir `/data/adb/ksud` antes da elevação de credenciais do processo (`escape_with_root_profile()`).
   - Como `/data/adb` possui permissão 0700 (root), a chamada falhava com `-EACCES` e ativava o fallback `pr_warn("ksud inaccessible, applying fallback to sh")`.
   - O aplicativo (ex: ZArchiver) executava `/system/bin/sh` em vez do binário `ksud`, falhando na negociação do protocolo root do KernelSU.
   - **Correção**: Removido o teste espúrio e restaurada a substituição direta por `ksud_path` com elevação `escape_with_root_profile()`, garantindo que todo `su` execute o `ksud`.

4. **Metamodule e scripts de inicialização não ativados no boot**:
   - O Android `init` moderno (Android 14/15/16/17 / InfinityX) lê arquivos de configuração através de `ReadFdToString` (`system/libbase/file.cpp`), que chama `fstat(fd, &sb)` e faz o loop de leitura `while (bytes_read < sb.st_size)`.
   - Como `newfstatat` (usado pelo ARM64 com `AT_EMPTY_PATH`) não ajustava o tamanho retornado de `init.rc`, o `init` parava de ler exatamente no fim do arquivo original, sem jamais ler o bloco injetado `KERNEL_SU_RC` (`exec ... /data/adb/ksud post-fs-data`).
   - Consequentemente, o estágio `post-fs-data` nunca era chamado pelo `init` após a reinicialização, mantendo os módulos em `/data/adb/modules_update` e o aviso "Pending changes: Reboot to apply changes first".
   - **Correção**: Adicionado o hook `ksu_handle_newfstat_ret` em `newfstatat` (`flag & AT_EMPTY_PATH`) e em `newfstat` em `fs/stat.c`, e adicionado `/etc/init/hw/init.rc` aos caminhos reconhecidos em `is_init_rc`. O `init` agora recebe `st_size = orig_size + ksu_rc_len` e lê o bloco `post-fs-data` completo.

5. **Símbolos indefinidos de SuSFS e linker**:
   - No `pstar-default.config`, garantido `CONFIG_KSU_SUSFS=y` junto com todos os sub-recursos (`SUS_PATH`, `SUS_MOUNT`, `SUS_KSTAT`, `SUS_MAP`, `OPEN_REDIRECT`, etc.), garantindo que `fs/susfs.o` seja compilado e linkado no `vmlinux`.
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
| `KernelSU-Next/kernel/Kbuild` | `KSU_MIN_COMPAT_VERSION 33333`, tag fallback `v3.4.1-legacy` |
| `KernelSU-Next/kernel/selinux/Makefile` | Adicionados os caminhos de include do diretório KernelSU pai para o build in-tree |
| `KernelSU-Next/kernel/selinux/sepolicy.c` | Backport das correções v3.4.1 de iteração avtab, contabilidade de `db->len` e atualização de permissões xperm |
| `KernelSU-Next/kernel/selinux/rules.c` | A correção RCU-protected SELinux policy do v3.4.1 já estava aplicada na árvore local |
| `KernelSU-Next/kernel/include/util.h` | A ligação `ksyscall` necessária para o kernel arm64 4.19 já estava aplicada na árvore local |

### Pasta exemplo/
```
/home/votikabr/Downloads/exemplos/KernelSU-Next-legacy/
  kernel/                        — Base legacy de KernelSU-Next para comparação
/home/votikabr/Downloads/exemplos/android_kernel_motorola_sm8250/
                                  — Snapshot de referência 33240 + SUSFS 2.3.0
/home/votikabr/Downloads/exemplos/kernel_patches-main/
  next/susfs_fix_patches/v2.2.0/ — Patches de referência SUSFS 2.2.0
/home/votikabr/Downloads/exemplos/susfs4ksu-gki-android/
                                  — Patches GKI; não aplicáveis diretamente ao pstar NON-GKI
```

### Lógica de Versão KSUN no Kbuild
- Em checkout KernelSU-Next separado: `30000 + git_commits + 289`, com piso em `33333`
- No kernel pstar, `KernelSU-Next` compartilha o repositório do kernel; o Kbuild usa o fallback **33333** e a tag `v3.4.1-legacy`

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

### SuSFS 2.3.0 Features (já integradas no kernel)
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

## Backport das correções da branch `legacy` (24/09 – 07/10/2026)

Merge 3-way (base: #1551 → ponta `8869bd7`) preservando as adaptações locais (SUSFS, hooks manuais, KSU_Toolkit):

- pin manager package name (#3868): `get_pkg_from_apk_dir_path`, `crown_manager`/`maybe_manager_apk_dir` em throne_tracker.c, log em init.c.
- Service stage (#3800/#1573): `EVENT_SERVICES`, UAPI versão 5.
- sucompat: fallback para `sh` até o ksud existir; presença do ksud mantida por observer em `/data/adb` (pkg_observer.c, boot_event.c). Local: a flag é usada também em contexto de processo (sem `open`, pois `/data/adb` é 0700).
- sucompat: execveat checa `PARM5` (flags); supercall: install-fd restrito a root/manager/su-allowed (`allowed_for_su`).
- selinux_hide (retry do hook, dedup/sync, ghost declaration, check_context por app-uid), app_profile, rules, dispatch, setuid_hook, apk_sign, includes/guards.
