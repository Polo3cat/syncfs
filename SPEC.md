# SPEC

## §G GOAL
Daemon keeps CWD identical across static peer set: inotify detects change → ZMQ PUB/SUB announces → libtorrent moves bytes.

## §C CONSTRAINTS
- C++23, clang++, CMake ≥ 4.3, `CMAKE_CXX_EXTENSIONS OFF`.
- Deps via CPM, cache `.cpm-cache`, pinned: fmt 11.1.4, spdlog 1.17.0 (`SPDLOG_FMT_EXTERNAL ON`), libzmq 4.3.5 + `patches/libzmq-cmake.patch`, cppzmq 4.11.0 + `patches/cppzmq-zmq_addon.patch`, Boost 1.91.0, libtorrent 2.1.0 (`webtorrent OFF`, `deprecated-functions OFF`, + §T.34 trim: `logging OFF`, `i2p OFF`, `streaming OFF`, `mutable-torrents OFF`, `build_tests/examples/tools OFF`, `python-bindings OFF`; `dht`/`encryption`/`extensions` stay ON — `protocol.cpp` announces DHT, `syncfs.cpp` loads `ut_pex`), googletest 1.17.0 (`INSTALL_GTEST OFF`).
- CPM deps ⊥ installed. `cmake --install` ! place exactly 1 file — `bin/syncfs`. ⊥ dep header, ⊥ dep archive, ⊥ dep `.pc`, ⊥ dep CMake config. Static link (V30) ∴ dep archives ⊥ needed @ runtime. Mechanism: `EXCLUDE_FROM_ALL YES` ∀ `cpmaddpackage` — CMake ≥ 3.28 skip excluded subdir @ install (⊥ @ generate, see §R.8). Was 17030 files before §T.33.
- syncfs ships as program, ⊥ CMake package. ⊥ export set, ⊥ `syncfsConfig.cmake`, ⊥ usage file, ⊥ `uninstall` target. ∴ ⊥ `find_package(syncfs)`. `cmake/PackageProject.cmake` deleted — it also pulled 2 unpinned configure-time zips (ycm, cmake-forward-arguments).
- Linux only — `sys/inotify.h`, `poll`.
- ∀ CPM dep link static into `syncfs` — fmt, spdlog, libzmq, cppzmq, Boost, torrent-rasterbar. `BUILD_SHARED_LIBS OFF` set @ top of `syncfs_setup_dependencies` (must beat `libtorrent/CMakeLists.txt:518` `feature_option(BUILD_SHARED_LIBS … ON)`, which write cache var & leak to ∀ dep on reconfigure). libzmq `BUILD_SHARED OFF` (own option, ignores `BUILD_SHARED_LIBS`) ∴ link target `cppzmq-static`, ⊥ `cppzmq`.
  System libs stay dynamic — libc, `libssl`/`libcrypto`, libatomic, libm, **libstdc++** (§T.34; `-static-libgcc` kept, but `libgcc_s` still loads behind `libstdc++.so`). ∴ NSS intact (V20), SHA-NI SHA-256 kept (T3 ⊥ regress), ASan/UBSan keep linking, OpenSSL CVE patched by distro update ⊥ rebuild.
  ∴ binary ⊥ portable across distro / glibc version. Run host ! provide matching libc + OpenSSL 3.
- Control plane ZMQ, data plane BitTorrent v2. ⊥ custom transport.
- ⊥ central server. Peer set static, read from file @ startup.
- Sync scope = CWD recursive. Regular non-symlink files only.
- File update semantics = last write wins. Newest `create` for a path replaces whatever torrent held that path. ⊥ conflict resolution, ⊥ version history, ⊥ merge.
- clang-tidy `Checks: "*"` minus listed, `-warnings-as-errors=*` (from `myproject_WARNINGS_AS_ERRORS` ON, `cmake/StaticAnalyzers.cmake:71`). ∴ ⊥ Boost.Asio in own sources — `boost/asio/io_context.hpp` trips `misc-header-include-cycle`.
- googletest added inside `setup_test_dependencies()` with `CMAKE_CXX_CLANG_TIDY`/`CPPCHECK` cleared @ function scope ∴ own sources exempt, `tests/` ⊥ exempt.
- Tests: GoogleTest (unit) + pytest ≥ 9.0.0, pytest-benchmark ≥ 5.2.3, pytest-repeat ≥ 0.9.4 (integration, performance), Python ≥ 3.13, poetry. Build & test run in `localhost/syncfs-env` (`fedora:44`, clang + ninja + ccache + lld) via podman (`Makefile`).

## §I INTERFACES
- cmd: `syncfs <peers file> <listen address>` → syncs CWD. args < 3 → usage on stdout, `EXIT_FAILURE`. usage line 1 = `Usage: syncf <peers file> <listen address>` (`syncf` typo, see §T.22).
- file: `<peers file>` — 1 address per line, form `tcp://host:port`, line ≤ 512 chars. opened read-only ∴ ⊥ write permission needed (V32).
- arg: `<listen address>` — `host:port`. ZMQ PUB binds `tcp://<listen address>`.
- net: SUB connects ∀ peer & self. Subscribes prefixes `create` & `remove`.
- net: libtorrent listens `0.0.0.0:<port+2000>`. `ut_pex` extension on. Peer discovery = DHT + LSD + PEX, ⊥ tracker.
- wire: 2-part multipart. part0 = verb. part1 = payload.
  - `create` → part1 = bencoded torrent, v2-only, 1 file, piece length 0 ∴ libtorrent auto-picks, sender libtorrent endpoint embedded as DHT node.
  - `remove` → part1 = file path.
- log: spdlog. `NDEBUG` → level `info`, pattern `[%Y-%m-%d %T] [%P] [%^%l%$] %v`. else level `debug`, `%T.%F`.
- stats: torrent table `Name Progr Total Seeds Peers State` @ `debug`, every 2 s.
- sig: `SIGTERM` & `SIGINT` → sync loop ends, `"Stopping."` @ `info`, `EXIT_SUCCESS` (V33).
- test: ctest names `utils-unit`, `source-unit`, `monitor-unit`, `protocol-unit`, `syncfs-integration`, `syncfs-performance`. `make {config,build,test,test-unit,test-integration,test-perfomance}` wrap them in podman (`test-perfomance` typo is the real target name).

## §R RESEARCH
libtorrent 2.1.0 behaviour established by debugging §B.1. Paths relative to `.cpm-cache/libtorrent/ea35609a2e1eb282111b2588b7910f375b683f92`.
id|topic|finding|src
R1|per-IP cap|`allow_multiple_connections_per_ip` defaults `false`. Many nodes behind 1 IP dedup to 1 peer, swarm starves. ! `true` whenever nodes share a host|src/settings_pack.cpp:134
R2|DHT node ≠ peer|`create_torrent::add_node` & `session::add_dht_node` register DHT routing entries, ⊥ bittorrent peers. Peers arrive via DHT lookup, LSD, PEX, tracker, `connect_peer`|include/libtorrent/torrent_handle.hpp:1309
R3|DHT on 1 host|`dht_restrict_routing_ips` defaults `true`, `routing_table.cpp:752` drops 2nd node sharing IP CIDR. Measured irrelevant to §B.1 — DHT found peers in ~50 ms with default kept|src/settings_pack.cpp:204
R4|explicit connect|`connect_peer` fix measured 0.151 s vs 0.051 s for R1 alone. Synchronous `getaddrinfo` in sync loop = 3× slowdown ∴ rejected|measured 2026-08-05, 10 nodes
R5|duplicate add|`add_torrent` on existing info hash returns existing handle & discards params. ∴ `add_torrent_params::peers` ⊥ applied on re-announce|include/libtorrent/add_torrent_params.hpp:305
R6|bootstrap|default `dht_bootstrap_nodes` = `dht.libtorrent.org:25401`, unreachable in test container|src/settings_pack.cpp:126
R7|auto-manage|`active_downloads` 3, `active_seeds` 5, `active_limit` 500, `connections_limit` 200. ⊥ binding @ 10 nodes|src/settings_pack.cpp:268-313
R8|install rules|`EXCLUDE_FROM_ALL` skip subdir `install()` @ install time, ⊥ @ generate time. libtorrent `install(EXPORT LibtorrentRasterbarTargets)` still generated ∴ require `boost_headers` in export set ∴ `BOOST_SKIP_INSTALL_RULES` ! stay `OFF` else generate fails `"which requires target boost_headers that is not in any export set"`|`.cpm-cache/libtorrent/…/CMakeLists.txt:1027`, measured 2026-08-07

## §V INVARIANTS
V1: args < 3 → usage & `EXIT_FAILURE`
V2: peers list ! non-empty (`assert` ∴ debug only ?)
V3: ∀ inbound msg → parts == 2 else `"Wrong protocol length."`
V4: verb ∉ {`create`, `remove`} → `"Wrong protocol verb."`
V5: ∀ generated torrent → `create_torrent::v2_only` & exactly 1 file
V6: ∀ torrent file path → relative. `file_absolute_path` == false
V7: ∀ added torrent → `add_torrent_params::save_path` set `"."` & `torrent_flags::auto_managed`. libtorrent resolves `"."` @ add ∴ `torrent_status::save_path` reads absolute CWD, ⊥ `"."` (§V.16). test `Protocol.V7AddedTorrentSavePathAndFlags`
V8: ∀ `create` received → `add_dht_node` ∀ `torrent.dht_nodes`, then `force_dht_announce`
V9: `parse_host_port` → empty host raises `std::invalid_argument`; empty port raises `std::invalid_argument` (`std::stoi("")`); ⊥ `:` raises `std::out_of_range` (`vector::at(1)`)
V10: `files::list` yields regular & ⊥ symlink & `last_write_time` readable only
V11: `files::list` ! materialize entries before filter ∴ ⊥ TOCTOU on `last_write_time` (`views::filter` caches begin iterator)
V12: libtorrent port == ZMQ port + 2000
V13: send order == `removed`, `created`, `modified`. modified → `create` only, ⊥ paired remove. `modified` entries come from `removed` (`intersection_name` copies left range) ∴ their `file_time_type` is the pre-change one, ⊥ current
V14: inotify mask == `IN_CLOSE_WRITE | IN_DELETE | IN_CREATE | IN_MOVED_FROM | IN_MOVED_TO | IN_DONT_FOLLOW | IN_EXCL_UNLINK`, same mask on `.` & ∀ subdirectory. `IN_CREATE`/`IN_MOVED_TO` ! present — new dir ⊥ emit `IN_CLOSE_WRITE` ∴ else undiscoverable; `IN_MOVED_FROM` ! present — renamed watched dir else leaves stale wd→path. test `Monitor.V14MaskAllowsSubdirectoryDiscovery`
V15: `Monitor` move-only. copy ctor & copy assign deleted. moved-from `fd == -1` & watch map empty
V16: 1 process == 1 sync root == CWD
V17: session ! set `allow_multiple_connections_per_ip = true` ∴ N nodes per host each hold own connection (see §R.1)
V18: 10 nodes, 4-byte change → converge ≤ 1 s. measured 0.051 s
V19: 2 nodes, 4-byte change → converge ≤ 5 s
V20: ∀ syncfs source → ⊥ blocking name resolution inside sync loop (see §R.4)
V21: changed set ! come from inotify event names, ⊥ full directory traversal per event — target, ⊥ yet true
V22: ∀ `Monitor::read()` → ! drain every queued event & ! survive filename up to `NAME_MAX`. read buffer ≥ `sizeof(inotify_event) + NAME_MAX + 1` (else `read()` `EINVAL`), 1 syscall parses ∀ event in it into a batch. tests `Monitor.V22SurvivesNameMaxFilename`, `Monitor.V22DrainsEveryQueuedEvent`
V23: inotify coverage ! match `files::list` recursion depth — 1 watch per directory, `.` + ∀ subdirectory, symlinked dir ⊥ followed (same default `recursive_directory_iterator` options as `files::list`). watch set ! resync on dir create / move / delete. `InotifyEvent::name` == path relative to sync root (`./a/b/f.txt`), ⊥ basename ∴ same key shape as `files::list`. tests `Monitor.V23WatchesEveryDirectoryAtConstruction`, `Monitor.V23EventInNestedDirectoryIsSeen`, `Monitor.V23NewSubdirectoryBecomesWatched`, `Monitor.V23RemovedDirectoryIsUnwatched`
V24: `torrent_info` on v2 torrent synthesizes tail pad ∴ `num_files()` == real + 1 when size % `piece_length` != 0. ∀ file-count assert ! read v2 `file tree` bencode, ⊥ `file_storage`
V25: ∀ received file → written path == sender relative path incl. ∀ parent dirs. ⊥ yet true @ depth 1 (see §B.3)
V26: `Monitor::wait()` poll timeout 50 ms & `Sink::receive_ready()` poll 100 ms, serial ∴ idle loop iteration ≤ 150 ms
V27: `wait()` true → `discard()` consumes whole drained batch, then full `files::list()` traversal drives diff ∴ 1 traversal per batch, ⊥ per event (current design, superseded by V21)
V28: ∀ fatal exception in `main` → `EXIT_FAILURE`. ∀ catch branch (`zmq::error_t` & `std::exception`) ! `return`, ⊥ fall off end of `main`. test `test_v28_fatal_zmq_error_exits_failure`
V29: ∀ `torrent_status::torrent_file.lock()` → null-check before deref. ⊥ yet true (see §T.24)
V30: `ldd syncfs` ⊥ name any CPM dep — ⊥ `libtorrent-rasterbar`, `libspdlog`, `libfmt`, `libzmq`, `libboost_*`. Allowed = distro libs: `libc`, `libm`, `libssl`, `libcrypto`, `libatomic`, `libbsd` + `libmd` (libzmq `strlcpy`), `libz` (libtorrent), `libstdc++` + `libgcc_s` (§T.34), `ld-linux`, `linux-vdso`. test `test_v30_no_cpm_dep_shared` & `test_v30_only_system_libs_shared`
V31: `cmake --install` → installed file set == exactly {`bin/syncfs`}. test `test_v31_only_syncfs_installed`
V32: ∀ read of `<peers file>` → open mode read-only (`std::ifstream`, ⊥ `std::fstream`) ∴ peer list from a non-writable file == peer list from a writable one. test `test_v32_read_only_peers_file_is_parsed`
V33: ∀ `SIGTERM` & `SIGINT` → `stop_requested` set, loop ends @ next iteration, exit code 0 ≤ 5 s. handler ! only assign `volatile std::sig_atomic_t`. ∴ ∀ poll in loop ! treat `EINTR` as "nothing ready", ⊥ throw — `Monitor::wait` → 0 events, `Sink::receive_ready` → false. test `test_v33_stop_signal_exits_success`
V34: `IN_CREATE` on non-directory ⊥ drive sync — `wait()` consumes it & reports "nothing ready". file still empty, writer holds it open, `IN_CLOSE_WRITE` follows. `IN_CREATE | IN_ISDIR` ! drive sync — dir gets no 2nd event. test `Monitor.V34FileCreationAloneDoesNotDriveSync`
V35: ∀ `create` received → ∃ torrent in session whose file path == incoming torrent file path → old handle removed (`session::remove_torrent`, ⊥ `remove_flags_t::delete_files`) before new added ∴ 1 path == 1 torrent. §R.5 dedup covers same info hash only; file update = new content = new info hash ∴ both handles would live & both claim same `save_path` file. path compare on §V.6 relative form ∴ holds @ depth 0 until §T.21 lands. info hash equal → ⊥ remove (identical re-announce ⊥ new content; remove+add would force recheck & drop swarm). tests `Protocol.V35NewTorrentReplacesSamePathTorrent`, `Protocol.V35IdenticalTorrentIsNotReadded`

V36: ∀ file libtorrent wrote → ⊥ republished while unchanged. `torrent_finished_alert` → `flush_cache()`; `cache_flushed_alert` → snapshot `path → last_write_time`. path dropped from diff `created`/`modified` when its snapshot == its mtime in the current `files::list()` ∴ ⊥ re-hash, ⊥ duplicate info hash on wire (§R.5). compare against `current`, ⊥ against the diff entry — `create_diff` builds `modified` from `removed` ∴ its entries carry the pre-change mtime & would match the snapshot always, swallowing every genuine edit (§V.13). later genuine edit → mtime differs → published. `alert_mask` ! include `alert_category::status` & `alert_category::storage`. snapshot @ `cache_flushed_alert`, ⊥ @ `torrent_finished_alert` — hash check ? precede final flush (`piece_finished_alert` docs). test `test_receiver_does_not_republish`

V37: ∀ `remove` received that deleted an existing file → path marked; next diff `removed` entry for it dropped & mark consumed ∴ ⊥ remove echo on wire. mark set only when file existed @ receipt ∴ ⊥ stale mark swallowing a later genuine delete. mirrors §V.36 for the create side. echo ⊥ only waste — path recreated between delete & echo → echo deletes new file. tests `test_receiver_does_not_republish_remove`, `test_delete_after_suppressed_remove_is_published`

V38: ∀ `remove` received → ∀ torrent in session whose file path == that path removed (`remove_torrent`, ⊥ `delete_files`) before the unlink ∴ ⊥ session seeds a vanished path & ⊥ read errors under a live torrent. unlink stays in `act` ∴ path gone deterministically when `act` returns (§V.37 mark logic depends on it). path compare on `lexically_normal` form — wire carries `files::list` key (`./f`), torrent carries normalized (`f`). mirror of §V.35 for the delete side. test `Protocol.V38RemoveDropsTorrentForPath`

## §T TASKS
id|status|task|cites
T1|x|fix `syncfs-update` non-convergence|V17,B1
T2|x|`source_test.cpp` asserted on `num_files()`, got 2 — pad. now asserts v2 `file tree`|V5,V24,B2
T3|.|`perf_one_to_one_gb_file_test` red — 1 GiB in 30 s budget, reaches 57%. Sender hashes whole file in `lt::set_piece_hashes`, synchronous, blocks sync loop|C,V20
T4|.|`tests/integration/many_syncfs_test.py:69` `test_one_syncf_sends_to_many_other` has ⊥ assert. loop breaks either way, test can never fail|V18
T5|x|downloader writes file → own inotify fires → republishes identical content → duplicate info hash, wasted hash & traffic. suppress echo|V13,R5,V36
T6|.|integration tests wait fixed `time.sleep`. flaky & slow. poll until deadline|V18,V19
T7|.|`README.md` still `cmake_template` boilerplate. rewrite for syncfs|G,I
T8|.|no unit test for `protocol::act`|V3,V4,V7,V8
T9|.|no unit test for `files.cpp` — `list`, `diff`, `diff_name`, `intersection_name`|V10,V11
T10|.|no unit test for `monitor.cpp`|V14,V15
T11|.|no unit test for `sink.cpp` — `receive` short-message path|-
T12|.|no test for `discovery::parse` — 512-char truncation, blank line, missing file|I
T13|.|`files::append` & `files::remove` defined, called nowhere. they are §T.15 incremental helpers|V21,T15
T14|x|dead commented block `src/monitor.cpp:116-128` dropped|-
T15|.|sync loop ! derive changed set from `Monitor::read()` events, drop full traversal. `read()` public again ∴ unblocked|V21,V27,T13
T19|x|`src/monitor.cpp:75` `buf_size` == `sizeof(inotify_event) * 4` == 64 B & `_read` returns first event only. rest of queue dropped silently, & `inotify(7)` fails `read()` `EINVAL` when buffer ⊥ hold next event ∴ filename ≥ ~47 chars breaks it. size buffer by `NAME_MAX`, loop over all events|V21,V22
T20|x|`src/monitor.cpp:24` watches `.` only, ⊥ recursive, but `files::list` is recursive. traversal hides gap today; event-driven would stop syncing subdirectories. watch per directory or use `fanotify`. `fanotify` rejected — `FAN_MARK_FILESYSTEM` needs `CAP_SYS_ADMIN`, run image is `USER syncfs` uid 1000 (T35). watch per dir + resync on dir create/move/delete, `monitor` now own static lib ∴ unit-testable|V21,V23,V14,V34,B7
T16|.|V2 relies on `assert` ∴ vanishes under `NDEBUG`. promote to real check. §B.5 = the release build silently peerless|V2,B5
T17|.|alert loop logs `alert->message()` @ debug but default `alert_mask` hides peer & DHT categories ∴ blind during B1 triage. widen mask|R2,R3
T18|.|node & file count ceiling undocumented|C
T21|.|v2-only torrent @ relative path depth 1 loses top dir on receiver. wire correct, load wrong. fix: carry relative path as 3rd wire part & derive `save_path`, or nest tree ≥ 2 deep|V25,B3
T22|.|`src/syncfs.cpp:193` usage prints `syncf`, binary is `syncfs`|I
T23|x|`src/syncfs.cpp:236` `zmq::error_t` catch logs & falls off end of `main` — ⊥ `return EXIT_FAILURE` ∴ exit code 0 on fatal ZMQ error|V28,B4
T24|.|`src/syncfs.cpp:97` `t.torrent_file.lock()` passed straight to `file_path`, deref'd unchecked. expired weak_ptr → nullptr deref in stats print|V29
T25|.|`src/monitor.cpp:118` `discard()` swallows `read()` error via `[[maybe_unused]]`. inotify read failure invisible|V22
T26|.|inotify poll 50 ms then ZMQ poll 100 ms, serial per iteration ∴ ≤ 150 ms idle latency & receive starves while waiting on inotify. single poll over both fds|V26
T27|.|`discovery::parse` on missing/unreadable file returns empty vector, ⊥ error. only `assert` catches. §B.5 shows the failure mode is real, ⊥ theoretical|V2,I,T12,T16,B5
T28|.|`tests/performance/perf_one_to_one_gb_file_test.py:96` test named `test_benchmark_sync_one_to_many`, is one-to-one|-
T29|x|CPM deps link shared. `BUILD_SHARED_LIBS OFF` @ top of `syncfs_setup_dependencies`, libzmq `BUILD_SHARED OFF`, swap `cppzmq` → `cppzmq-static` in `src/CMakeLists.txt`, `-static-libstdc++ -static-libgcc` on `syncfs`, `libstdc++-static` in `Containerfile`. verify `ldd`. (`-static-libstdc++` + `libstdc++-static` reverted @ §T.34)|V30,C
T30|.|`tests/integration/CMakeLists.txt:6` & `tests/performance/CMakeLists.txt:6` set `PATH=$<TARGET_FILE_DIR:syncfs>:${CMAKE_SYSTEM_PREFIX_PATH}`. `CMAKE_SYSTEM_PREFIX_PATH` is a `;`-list of prefixes (`/usr/local;/usr`), ⊥ a PATH ∴ `/usr/bin` never reachable. only `syncfs` resolves; ∀ other tool need absolute path (see `static_link_test.py::find_ldd`)|I,V30
T31|.|poetry venv location differ build vs test. `poetry-install` target write `/root/.cache/pypoetry/virtualenvs` (container-local, lost on image rebuild); ctest env set `POETRY_VIRTUALENVS_IN_PROJECT=true` ∴ read `.venv` on mounted volume. fresh image → `No module named pytest`. pin one location|I,T6
T32|x|§I listed ctest name `syncfs-update` & `make test-update`, neither exist. resolved §I side: both dropped, `test-integration` added. ⊥ new test written — update coverage lives in `syncfs-integration` + `protocol-unit` §V.35|I,V35
T33|x|dep `install()` rules leaked ∴ 17030 files in prefix. `EXCLUDE_FROM_ALL YES` ∀ `cpmaddpackage`, drop `syncfs_package_project` for plain `install(TARGETS syncfs RUNTIME …)`, delete `cmake/PackageProject.cmake`. now 1 file|V31,C,R8
T34|x|installed binary 101 MB. 94 MB of it = `.debug_*` (preset build type `RelWithDebInfo`), `.text` 5.2 MB (libtorrent 3.4 MB, libstdc++ 1.2 MB). fixes: `cmake --install --strip`; `-ffunction-sections -fdata-sections` in top `CMakeLists.txt` before `include(Dependencies.cmake)` (deps sectioned too — `--gc-sections` cuts @ section granularity, LTO prunes only inside its graph) + `-fuse-ld=lld -Wl,--gc-sections -Wl,--icf=safe` on `syncfs`; libtorrent feature trim; drop `-static-libstdc++`. `--icf=safe` ⊥ `all`: `all` folds address-taken fns ∴ fn-pointer compare silently true; `safe` reads clang `.llvm_addrsig`. result 101 MB → 4.3 MB, `.text` 5.2 → 3.9 MB|V30,C
T35|x|runtime image. `Containerfile.run` 2 stage: build stage compiles from sources bind-mounted @ `/syncfs` (⊥ `COPY` ∴ ⊥ source in any layer), run stage = `fedora:44` + only the V30 distro libs + `bin/syncfs`, `USER syncfs` uid 1000, `WORKDIR /data`. preset `unixlike-clang-dist` (inherits release, sanitizers/clang-tidy/cppcheck/ccache/`BUILD_TESTING` OFF). `make run-image`. layer cache ⊥ see a mounted source ∴ `ARG SOURCE_ID` busts the build layer per invocation. 203 MB, 190 MB of it the base|C,V30,V31
T36|x|`discovery::parse` used `std::fstream` ∴ read-only peers file = empty peer list, silent|V32,B5
T37|x|no `SIGTERM`/`SIGINT` handling ∴ ⊥ stoppable as container PID 1. flag + `EINTR` tolerated in both polls|V33,B6
T38|x|`src/protocol.cpp:58` `add_torrent` blind — no lookup of existing torrent for same path. file update leaves stale torrent seeding old content on same `save_path`. scan `session::get_torrents()` (or keep path→handle map) & `remove_torrent` old before add|V35,R5,V13,T5
T39|x|`remove` echo — receiver deletes file on `remove`, own `IN_DELETE` fires, republishes `remove`. O(N²) messages per delete & echo deletes a path recreated meanwhile|V13,V36,V37
T40|x|`src/protocol.cpp` `remove` branch deletes file & leaves torrent in session ∴ session seeds a path that no longer exists. `remove_torrent` it, mirror of §V.35|V35,V37,V38

## §B BUGS
id|date|cause|fix
B1|2026-08-05|`allow_multiple_connections_per_ip` default `false` ∴ 10 nodes on 127.0.0.1 dedup to 1 peer. seeder held 0 connections for 20 s, `syncfs-update` never converged. ⊥ DHT latency, ⊥ libtorrent throughput limit|V17
B2|2026-08-05|`file_storage::add_file_borrow` (`src/file_storage.cpp:939`) appends tail pad `.pad/<n>` ∀ v2 file when `total_size % piece_length != 0` ∴ 1-file torrent loads as `num_files() == 2`. ⊥ load flag suppresses it; `remove_tail_padding` reachable only via hybrid branch `torrent_info.cpp:1348`|V24
B3|2026-08-05|v2-only single-file-in-subdir torrent drops top dir on load. `parse_info_section` passes `is_multi_file = bool(files_node)`; `v2_only` ∴ ⊥ `files` list ∴ false. `extract_files2` single_file shortcut (`src/torrent_info.cpp:697`) then discards `root_dir` (= info `name`). `/tmp/important_file` loads as `important_file`|V25
B4|2026-08-06|`zmq::error_t` catch @ `src/syncfs.cpp:236` logged & fell off end of `main` ∴ implicit `return 0`. Fatal bind `EADDRINUSE` reported success to shell & CI. `std::exception` sibling branch had the `return`, zmq one ⊥|V28
B5|2026-08-08|`discovery::parse` opened peers file `std::fstream` — default mode `in\|out` ∴ open fails on a non-writable file, `getline` fails @ once, empty peer list. `assert(!peers.empty())` gone under `NDEBUG` (T16) ∴ daemon started, bound PUB, synced with nobody, only log `Subscribed to … (myself)`. Found running the container image with peers file mounted `:ro`. Tests ⊥ caught it: they write peers to a `NamedTemporaryFile` & run as root (root opens 0444 rw) ∴ V32 test ! drop privileges|V32,T27
B6|2026-08-08|As PID 1 of a container `SIGTERM` has no default disposition ∴ unhandled = ignored. `podman stop` waited 10 s then `SIGKILL`, sync loop `while (true)` had no exit. Handling it alone ⊥ enough: signal lands during a poll, `monitor::_wait` threw on `poll` `-1` & `Sink::receive_ready` let `zmq::error_t` out ∴ `EINTR` would have exited `EXIT_FAILURE` down the `main` catch|V33
B7|2026-08-08|T20 widened inotify mask with `IN_CREATE` (needed — new dir emits no `IN_CLOSE_WRITE`) ∴ receiver woke @ file create, ⊥ only @ close. loop then ran `files::list` + `source::create` on the 1 GiB file libtorrent had just created ∴ `lt::set_piece_hashes` hashed a partial file inside the sync loop, ×10 nodes. `perf_one_to_one_gb_file_test` 97 s → hit its 120 s deadline. Filtering `IN_CREATE` out was blocked by T19: 64 B buffer, `_read` returned first event only, and the paired `IN_CLOSE_WRITE` sat in the same discarded buffer ∴ T19 ! land first|V34,V22
