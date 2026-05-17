#include "nix/store/build/find-cycles.hh"

// FIXME also find references to hash or hash-name
// a reference to a derivation hash alone is enough to form a cycle edge
// if we only find hash or hash-name
// then we have to "to" path
// so that is the end of a cycle

// FIXME the error message should show the raw matches
// and maybe resolved derivation paths like
// - hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh -> /nix/store/hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh-x
// or
// - hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh
// = /nix/store/hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh-x

// FIXME also find relative paths like "../hash-name" instead of "/nix/store/hash-name"

// FIXME test this with single-file derivations
// where for example $out is a file

// FIXME cycles should try to start with the first output ("out")
// the first and second edge of the cycle should be sorted in the same order as the derivation's outputs
// outputs = [ "out" "dev" "bin" ]; # out -> dev -> bin -> out
// so in some cases, we have to shift or invert the cycle

#include <algorithm>
#include <exception>
#include <filesystem>
#include <map>

namespace nix {

CycleEdgeScanSink::CycleEdgeScanSink(StringSet && hashes, std::string storeDir)
    : RefScanSink(std::move(hashes))
    , storeDir(std::move(storeDir))
{
}

void CycleEdgeScanSink::setCurrentPath(const std::string & path)
{
    currentFilePath = path;
    // Clear tracking for new file
    recordedForCurrentFile.clear();
}

void CycleEdgeScanSink::operator()(std::string_view data)
{
    // Call parent's operator() to do the actual hash searching
    // This reuses all the proven buffer boundary handling logic
    RefScanSink::operator()(data);

    // Check which hashes have been found and not yet recorded for this file
    // getResult() returns the set of ALL hashes found so far
    for (const auto & hash : getResult()) {
        if (recordedForCurrentFile.insert(hash).second) {
            // This hash was just found and not yet recorded for current file
            // Create an edge from current file to the target
            auto targetPath = storeDir + hash;

            edges.push_back({currentFilePath, targetPath});

            debug("found cycle edge: %s → %s (hash: %s)", currentFilePath, targetPath, hash);
        }
    }
}

StoreCycleEdgeVec && CycleEdgeScanSink::getEdges()
{
    return std::move(edges);
}

void scanForCycleEdges(
    LocalStore & store,
    SourceAccessor & accessor,
    size_t chrootPrefixLen,
    const std::string chrootPrefix,
    const CanonPath & path,
    const StorePathSet & refs,
    StoreCycleEdgeVec & edges
)
{
    std::map<std::string, StorePath> hashPathMap;

    StringSet hashes;

    for (auto & i : refs) {
        std::string hashPart(i.hashPart());
        hashPathMap.emplace(hashPart, i);
        hashes.insert(hashPart);
    }

    scanForCycleEdges2(
        store,
        accessor,
        chrootPrefixLen,
        chrootPrefix,
        path,
        // path.abs(),
        hashPathMap,
        edges
    );
}

void scanForCycleEdges2(
    LocalStore & store,
    SourceAccessor & accessor,
    size_t chrootPrefixLen,
    const std::string chrootPrefix,
    const CanonPath & path,
    const std::map<std::string, StorePath> & hashPathMap,
    StoreCycleEdgeVec & edges
)
{
    auto info = accessor.lstat(path);
    if (info.type == SourceAccessor::tDirectory) {
        debug("scanForCycleEdges2: path is a directory: %s", path);
        for (const auto & [name, type] : accessor.readDirectory(path)) {
            scanForCycleEdges2(
                store,
                accessor,
                chrootPrefixLen,
                chrootPrefix,
                path / name,
                hashPathMap,
                edges);
        }
        return;
    }
    // FIXME this should be a bytestring (?)
    std::string content;
    if (info.type == SourceAccessor::tSymlink) {
        debug("scanForCycleEdges2: path is a symlink: %s", path);
        content = accessor.readLink(path);
    } else if (info.type == SourceAccessor::tRegular) {
        debug("scanForCycleEdges2: path is a file: %s", path);
        auto file = accessor.readFile(path);
        if (file.empty())
            return;
        content = file;
    } else {
        debug("scanForCycleEdges2: path is of unknown type: %s", path);
        return;
    }
    for (auto & [hash, targetStorePath] : hashPathMap) {
        // TODO also check filename
        if (content.find(hash) == std::string::npos)
            continue;
        /*
        FIXME the "from" path is wrong
        example:
        scanForCycleEdges2: found hash in path:
        path=/subdir/out-to-bin-2
        hash=nim5yyh540r583888k7fjnmphn5nw3j1
        from=/nix/store/nim5yyh540r583888k7fjnmphn5nw3j1-cyclic-outputs-bin
        to=/nix/store/nim5yyh540r583888k7fjnmphn5nw3j1-cyclic-outputs-bin
        */

        // auto it = hashPathMap.find(hash);
        // if (it == hashPathMap.end())
        //     continue;
        // StorePath fromStore = it->second;
        // const auto from = store.printStorePath(fromStore);

        // actual: from=/nix/store/rngknmkywf75sh5i5pwpd66kz59xkx0a-cyclic-outputs.drv.chroot/root/nix/store/7k4kll9ph61i9s0l1767gkd8ykk731xj-cyclic-outputs/subdir/out-to-bin-2
        // expected: from=/nix/store/7k4kll9ph61i9s0l1767gkd8ykk731xj-cyclic-outputs/subdir/out-to-bin-2
        auto from = accessor.showPath(path);

        // remove the chroot prefix path before "/nix/store/"
        // TODO better?
        if (chrootPrefixLen > 0 && from.size() >= chrootPrefixLen)
            from.erase(0, chrootPrefixLen);

        // error: ‘const class nix::CanonPath’ has no member named ‘toString’
        // auto from = path.toString();

        // // actual: from=/nix/store//subdir/out-to-bin-2
        // // expected: from=/nix/store/7k4kll9ph61i9s0l1767gkd8ykk731xj-cyclic-outputs/subdir/out-to-bin-2
        // const std::string storeRoot = "/nix/store";
        // std::string from = storeRoot + "/" + path.abs();

        const auto to = store.printStorePath(targetStorePath);
        // to=/nix/store/l6jk0s32idk8pdr5wi0kzj19blvkiyky-cyclic-outputs-dev
        debug("scanForCycleEdges2: found hash in path: path=%s hash=%s from=%s to=%s -> trying to find the actual file path in 'to'",
            path,
            hash,
            from,
            to
        );
        // edges.push_back(StoreCycleEdge{from, to});
        // try to find the actual file path in "to"
        // const auto storePathPrefix = store.printStorePath(actualPath); // missing: actualPath
        // debug("scanForCycleEdges2: storePathPrefix=%s", storePathPrefix);

        // debug("scanForCycleEdges2: found hash in path: to=%s parseStorePath=%s toRealPath=%s",
        //     to,
        //     store.parseStorePath(to).to_string(),
        //     std::string(store.toRealPath(store.parseStorePath(to)))
        // );

        // to=/nix/store/l6jk0s32idk8pdr5wi0kzj19blvkiyky-cyclic-outputs-dev
        // parseStorePath=l6jk0s32idk8pdr5wi0kzj19blvkiyky-cyclic-outputs-dev
        // toRealPath=/nix/store/l6jk0s32idk8pdr5wi0kzj19blvkiyky-cyclic-outputs-dev

        std::optional<ref<SourceAccessor>> targetAccessor;
        try {

            // error: ‘realPathInHost’ was not declared in this scope
            // virtual std::filesystem::path realPathInHost(const std::filesystem::path & p)
            // {
            //     return store.toRealPath(store.parseStorePath(p.native()));
            // }
            // targetAccessor = makeFSSourceAccessor(realPathInHost(to));

            // FIXME failed to get targetAccessor: error: opening file "/nix/store/l6jk0s32idk8pdr5wi0kzj19blvkiyky-cyclic-outputs-dev": No such file or directory
            // quickfix: add chrootPrefix
            // why does makeFSSourceAccessor fail to add chrootPrefix?
            targetAccessor = makeFSSourceAccessor(
                // store.toRealPath(store.parseStorePath(to))
                chrootPrefix + std::string(store.toRealPath(store.parseStorePath(to)))
            );
        }
        catch (std::exception & e) {
            debug("failed to get targetAccessor: %s", e.what());
            throw;
        }

        debug("targetAccessor.pathExists(\"/\")=%s", (**targetAccessor).pathExists(CanonPath("/")));

        for (size_t startPos = 0; startPos < content.size(); ++startPos) {
            // paths start with '/' or '.'
            if (content[startPos] != '/' && content[startPos] != '.')
                continue;
            debug("scanForCycleEdges2: calling findLongestExistingStorePath");
            auto maybePath = findLongestExistingStorePath(
                // FIXME error: invalid initialization of reference of type ‘nix::SourceAccessor&’ from expression of type ‘nix::ref<nix::SourceAccessor>’
                **targetAccessor,
                content,
                startPos,
                to
            );
            if (!maybePath) {
                debug("scanForCycleEdges2: no targetPath");
                continue;
            }
            const auto & targetPath = *maybePath;
            debug("scanForCycleEdges2: targetPath=%s", targetPath);
            // verify this is one of our refs
            for (auto & [hash, targetStorePath] : hashPathMap) {
                auto storePath = store.printStorePath(targetStorePath);
                // debug("scanForCycleEdges2: storePath=%s", storePath);
                if (!targetPath.starts_with(storePath)) {
                    // debug("scanForCycleEdges2: targetPath=%s does not start with storePath=%s", targetPath, storePath);
                    continue;
                }
                debug("scanForCycleEdges2: targetPath=%s starts with storePath=%s", targetPath, storePath);
                // /nix/store/rngknmkywf75sh5i5pwpd66kz59xkx0a-cyclic-outputs.drv.chroot/root/nix/store/nim5yyh540r583888k7fjnmphn5nw3j1-cyclic-outputs-bin/subdir/bin-to-dev-2
                auto from = accessor.showPath(path);

                // remove the chroot prefix path before "/nix/store/"
                // TODO better?
                if (chrootPrefixLen > 0 && from.size() >= chrootPrefixLen)
                    from.erase(0, chrootPrefixLen);
                    // /nix/store/nim5yyh540r583888k7fjnmphn5nw3j1-cyclic-outputs-bin/subdir/bin-to-dev-2

                debug(
                    "scanForCycleEdges2: cycle edge:\n"
                    "  from=%s\n"
                    "  to=%s",
                    from,
                    targetPath);
                edges.push_back(StoreCycleEdge{from, targetPath});
                break;
            }
        }
    }
}

/**
 * Recursively walk filesystem and stream files into the sink.
 * This reuses RefScanSink's hash-finding logic instead of reimplementing it.
 */
void walkAndScanPath(
    SourceAccessor & accessor, const CanonPath & path, const std::string & displayPath, CycleEdgeScanSink & sink)
{
    auto stat = accessor.lstat(path);

    debug("walkAndScanPath: scanning path = %s", displayPath);

    switch (stat.type) {
    case SourceAccessor::tRegular: {
        // Handle regular files - stream contents into sink
        sink.setCurrentPath(displayPath);
        accessor.readFile(path, sink);
        break;
    }

    case SourceAccessor::tDirectory: {
        // Handle directories - recursively scan contents
        auto entries = accessor.readDirectory(path);
        for (const auto & [name, entryType] : entries) {
            auto childPath = path / name;
            auto childDisplayPath = displayPath + "/" + name;
            debug("walkAndScanPath: recursing into %s", childDisplayPath);
            walkAndScanPath(accessor, childPath, childDisplayPath, sink);
        }
        break;
    }

    case SourceAccessor::tSymlink: {
        // Handle symlinks - stream link target into sink
        auto linkTarget = accessor.readLink(path);

        debug("walkAndScanPath: scanning symlink %s -> %s", displayPath, linkTarget);

        sink.setCurrentPath(displayPath);
        sink(std::string_view(linkTarget));
        break;
    }

    case SourceAccessor::tChar:
    case SourceAccessor::tBlock:
    case SourceAccessor::tSocket:
    case SourceAccessor::tFifo:
    case SourceAccessor::tUnknown:
    default:
        throw Error("file '%1%' has an unsupported type", displayPath);
    }
}

void transformEdgesToMultiedges(
    StoreCycleEdgeVec & edges,
    StoreCycleEdgeVec & multiedges)
{
    // adjacency: from -> to list
    std::multimap<std::string, std::string> graph;

    for (const auto & e : edges) {
        graph.emplace(e.from, e.to);
    }

    std::set<std::string> usedEdges;

    for (const auto & startEdge : edges) {

        StoreCycleEdgeVec path;
        std::string start = startEdge.from;
        std::string current = start;

        std::vector<std::string> nodes;
        nodes.push_back(start);

        std::set<std::string> seenNodes;
        seenNodes.insert(start);

        while (true) {

            auto range = graph.equal_range(current);

            bool advanced = false;

            for (auto it = range.first; it != range.second; ++it) {

                std::string next = it->second;

                std::string edgeKey = current + "->" + next;

                if (usedEdges.count(edgeKey))
                    continue;

                usedEdges.insert(edgeKey);

                nodes.push_back(next);

                current = next;
                advanced = true;

                break;
            }

            if (!advanced)
                break;

            if (current == start && nodes.size() > 1) {

                StoreCycleEdgeVec cycle;

                for (size_t i = 0; i < nodes.size(); i++) {
                    if (i + 1 < nodes.size()) {
                        cycle.push_back(StoreCycleEdge{
                            nodes[i],
                            nodes[i + 1]
                        });
                    }
                }

                multiedges.insert(
                    multiedges.end(),
                    cycle.begin(),
                    cycle.end());

                break;
            }
        }
    }
}

std::optional<std::string> findLongestExistingStorePath(
    SourceAccessor & accessor,
    const std::string & content,
    const size_t startPos,
    // the "to" derivation's outPath: "/nix/store/hash-name"
    const std::string & storePathPrefix
)
{
    // FIXME why? in theory, there is no upper bound...
    constexpr size_t maxPathLen = 4096;

    debug("findLongestExistingStorePath: start=%d: storePathPrefix=%s", startPos, storePathPrefix);

    std::optional<std::string> best;

    size_t end = 0;

    // relative path: "../hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh-x" // 37 bytes
    // absolute path: "/nix/store/hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh-x" // 45 bytes
    const size_t minPathLen = 37;

    for (
        end = startPos + minPathLen;
        end < content.size() && end - startPos < maxPathLen;
        ++end
    )
    {
        unsigned char c = content[end];

        if (c == '\0') {
            debug("findLongestExistingStorePath: start=%d: end=%d: null byte", startPos, end);
            break;
        }

        if (c < 32 && c != '\t' && c != '\n') {
            debug("findLongestExistingStorePath: start=%d: end=%d: bad char: %c", startPos, end, c);
            break;
        }

        std::string raw =
            content.substr(startPos, end - startPos);

        // debug("findLongestExistingStorePath: start=%d: end=%d: raw=%s", startPos, end, raw);

        try {

            auto normalized =
                std::filesystem::weakly_canonical(raw).string();

            // debug("findLongestExistingStorePath: start=%d: end=%d: raw=%s norm=%s", startPos, end, raw, normalized);

            if (normalized.ends_with("/")) {
                // remove trailing slash
                normalized = normalized.substr(0, normalized.size() - 1);
                // debug("findLongestExistingStorePath: start=%d: end=%d: raw=%s norm=%s -> removed trailing '/'", startPos, end, raw, normalized);
            }

            // debug("findLongestExistingStorePath: normalized=%s", normalized);

            // FIXME handle relative paths like "../hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh-x"
            // so at least normalized must include "/hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh-x"

            // must belong to this output
            if (!normalized.starts_with(storePathPrefix)) {
                // debug("findLongestExistingStorePath: start=%d: end=%d: raw=%s norm=%s -> wrong prefix", startPos, end, raw, normalized);
                continue;
            }

            // convert:
            // /nix/store/hash-name/foo/bar
            // ->
            // /foo/bar
            std::string rel = normalized.substr(storePathPrefix.size());

            if (rel.empty()) {
                rel = "/";
            }

            CanonPath canon(rel);

            // alternative to accessor.pathExists
            // FIXME findLongestExistingStorePath: start=0: end=80: raw=/nix/store/l6jk0s32idk8pdr5wi0kzj19blvkiyky-cyclic-outputs-dev/subdir/dev-to-bin rel=/subdir/dev-to-bin -> no such file

            if (raw == "/nix/store/l6jk0s32idk8pdr5wi0kzj19blvkiyky-cyclic-outputs-dev/subdir/dev-to-bin") {
                // FIXME findLongestExistingStorePath: start=0: end=80:
                // raw=/nix/store/l6jk0s32idk8pdr5wi0kzj19blvkiyky-cyclic-outputs-dev/subdir/dev-to-bin
                // exc=error: path '/nix/store/rngknmkywf75sh5i5pwpd66kz59xkx0a-cyclic-outputs.drv.chroot/root
                // /nix/store/7k4kll9ph61i9s0l1767gkd8ykk731xj-cyclic-outputs/subdir/dev-to-bin' does not exist
                // -> ignoring malformed candidate

                // wrong accessor?!
                // expected: dev = /nix/store/l6jk0s32idk8pdr5wi0kzj19blvkiyky-cyclic-outputs-dev
                // actual:   out = /nix/store/7k4kll9ph61i9s0l1767gkd8ykk731xj-cyclic-outputs
                auto info = accessor.lstat(canon);
                debug("findLongestExistingStorePath: start=%d: end=%d: raw=%s rel=%s -> info.type=%d", startPos, end, raw, rel, info.type);
            }

            if (accessor.pathExists(canon)) {
                if (!best || normalized.size() > best->size()) {
                    // debug("findLongestExistingStorePath: start=%d: end=%d: raw=%s norm=%s rel=%s -> exists + longer than best", startPos, end, raw, normalized, rel);
                    debug("findLongestExistingStorePath: start=%d: end=%d: raw=%s rel=%s -> exists + longer than best", startPos, end, raw, rel);
                    best = normalized;
                }
                else {
                    // debug("findLongestExistingStorePath: start=%d: end=%d: raw=%s norm=%s rel=%s -> exists + shorter than best", startPos, end, raw, normalized, rel);
                    debug("findLongestExistingStorePath: start=%d: end=%d: raw=%s rel=%s -> exists + shorter than best", startPos, end, raw, rel);
                }
            }
            else {
                // debug("findLongestExistingStorePath: start=%d: end=%d: raw=%s norm=%s rel=%s -> no such file", startPos, end, raw, normalized, rel);
                debug("findLongestExistingStorePath: start=%d: end=%d: raw=%s rel=%s -> no such file", startPos, end, raw, rel);
            }
        }
        catch (std::exception & e) {
            // ignore malformed candidates
            debug("findLongestExistingStorePath: start=%d: end=%d: raw=%s exc=%s -> ignoring malformed candidate", startPos, end, raw, e.what());
        }
    }

    debug("findLongestExistingStorePath: start=%d: end=%d: done", startPos, end);

    return best;
}

} // namespace nix
