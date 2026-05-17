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

            debug("found cycle edge: %s -> %s (hash: %s)", currentFilePath, targetPath, hash);
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
        content = accessor.readLink(path);
        // content can be anything, so we escape it to a JSON string
        debug("scanForCycleEdges2: path is a symlink: %s -> content=%s", path, nlohmann::json(content).dump());
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
                from,
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

void transformEdgesToMultiedges(StoreCycleEdgeVec & edges, StoreCycleEdgeVec & multiedges)
{
    debug("transformEdgesToMultiedges: processing %lu edges", edges.size());

    // Maps to track path endpoints for efficient joining
    // Key: node name, Value: index into multiedges vector
    std::map<std::string, size_t> pathStartingAt; // Maps start node -> path index
    std::map<std::string, size_t> pathEndingAt;   // Maps end node -> path index

    for (auto & edge : edges) {
        if (edge.empty())
            continue;

        const std::string & edgeStart = edge.front();
        const std::string & edgeEnd = edge.back();

        // Check if this edge can connect to existing paths
        auto startIt = pathEndingAt.find(edgeStart);
        auto endIt = pathStartingAt.find(edgeEnd);

        bool canPrepend = (startIt != pathEndingAt.end());
        bool canAppend = (endIt != pathStartingAt.end());

        if (canPrepend && canAppend && startIt->second == endIt->second) {
            // Edge connects a path to itself - append it to form a cycle
            size_t pathIdx = startIt->second;
            auto & path = multiedges[pathIdx];
            // Append all but first element of edge (first element is duplicate)
            path.insert(path.end(), std::next(edge.begin()), edge.end());
            // Update the end point (start point stays the same for a cycle)
            pathEndingAt.erase(startIt);
            pathEndingAt[edgeEnd] = pathIdx;
        } else if (canPrepend && canAppend) {
            // Edge joins two different paths - merge them
            size_t prependIdx = startIt->second;
            size_t appendIdx = endIt->second;
            auto & prependPath = multiedges[prependIdx];
            auto & appendPath = multiedges[appendIdx];

            // Save endpoint before modifying appendPath
            const std::string appendPathEnd = appendPath.back();
            const std::string appendPathStart = appendPath.front();

            // Append edge (without first element) to prependPath
            prependPath.insert(prependPath.end(), std::next(edge.begin()), edge.end());
            // Append appendPath (without first element) to prependPath
            prependPath.insert(prependPath.end(), std::next(appendPath.begin()), appendPath.end());

            // Update maps: prependPath now ends where appendPath ended
            pathEndingAt.erase(startIt);
            pathEndingAt[appendPathEnd] = prependIdx;
            pathStartingAt.erase(appendPathStart);

            // Mark appendPath for removal by clearing it
            appendPath.clear();
        } else if (canPrepend) {
            // Edge extends an existing path at its end
            size_t pathIdx = startIt->second;
            auto & path = multiedges[pathIdx];
            // Append all but first element of edge (first element is duplicate)
            path.insert(path.end(), std::next(edge.begin()), edge.end());
            // Update the end point
            pathEndingAt.erase(startIt);
            pathEndingAt[edgeEnd] = pathIdx;
        } else if (canAppend) {
            // Edge extends an existing path at its start
            size_t pathIdx = endIt->second;
            auto & path = multiedges[pathIdx];
            // Prepend all but last element of edge (last element is duplicate)
            path.insert(path.begin(), edge.begin(), std::prev(edge.end()));
            // Update the start point
            pathStartingAt.erase(endIt);
            pathStartingAt[edgeStart] = pathIdx;
        } else {
            // Edge doesn't connect to anything - start a new path
            size_t newIdx = multiedges.size();
            multiedges.push_back(edge);
            pathStartingAt[edgeStart] = newIdx;
            pathEndingAt[edgeEnd] = newIdx;
        }
    }

    // Remove empty paths (those that were merged into others)
    multiedges.erase(
        std::remove_if(multiedges.begin(), multiedges.end(), [](const StoreCycleEdge & p) { return p.empty(); }),
        multiedges.end());

    debug("transformEdgesToMultiedges: result has %lu multiedges", multiedges.size());
}

std::optional<std::string> findLongestExistingStorePath(
    SourceAccessor & accessor,
    const std::string & content,
    const size_t startPos,
    const std::string & from,
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

        std::filesystem::path fromDir = std::filesystem::path(from).parent_path();

        // resolve relative paths relative to fromDir
        std::filesystem::path joined = fromDir / raw;

        // debug("findLongestExistingStorePath: start=%d: end=%d: raw=%s fromDir=%s joined=%s", startPos, end, raw, std::string(fromDir), std::string(joined));

        try {

            auto normalized =
                std::filesystem::weakly_canonical(joined).string();

            // debug("findLongestExistingStorePath: start=%d: end=%d: raw=%s norm=%s", startPos, end, raw, normalized);

            if (normalized.ends_with("/")) {
                // remove trailing slash
                normalized = normalized.substr(0, normalized.size() - 1);
                // debug("findLongestExistingStorePath: start=%d: end=%d: raw=%s norm=%s -> removed trailing '/'", startPos, end, raw, normalized);
            }

            // debug("findLongestExistingStorePath: normalized=%s", normalized);

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

bool isCycleError(const BuildError & error)
{
    std::string originalMsg;
    try {
        auto &be = dynamic_cast<const BuildError &>(error);
        originalMsg = be.msg();
    } catch (...) {
        originalMsg = error.what();
    }
    for (auto prefix : {
            ANSI_RED "error:" ANSI_NORMAL " cycle detected in build of",
            "error: cycle detected in build of"
        })
    {
        if (originalMsg.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

BuildError getDetailedCycleError(const CycleErrorContext & ctx)
{
    if (!isCycleError(ctx.error)) {
        return ctx.error;
    }

    debug("getting detailed cycle error for %s", ctx.stageName);

    // Scan all outputs for cycle edges with exact file paths
    StoreCycleEdgeVec edges;

    for (std::vector<std::string> & outputItem : ctx.scanOutputs()) {

        std::string outputName = outputItem[0];

        // /nix/store/nim5yyh540r583888k7fjnmphn5nw3j1-cyclic-outputs-bin
        std::string actualPath = outputItem[1];

        // /nix/store/rngknmkywf75sh5i5pwpd66kz59xkx0a-cyclic-outputs.drv.chroot/root/nix/store/nim5yyh540r583888k7fjnmphn5nw3j1-cyclic-outputs-bin
        std::string hostPath = outputItem[2];

        debug("scanning for cycle edges in output '%s' at path '%s'", outputName, PathFmt(actualPath));

        // FIXME chroot prefix is missing in hostPath2
        // hostPath=/nix/store/rngknmkywf75sh5i5pwpd66kz59xkx0a-cyclic-outputs.drv.chroot/root/nix/store/nim5yyh540r583888k7fjnmphn5nw3j1-cyclic-outputs-bin
        // hostPath2=/nix/store/nim5yyh540r583888k7fjnmphn5nw3j1-cyclic-outputs-bin
        // auto hostPath2 = std::string(realPathInHost(actualPath));
        // auto hostPath2 = std::string(ctx.store.toRealPath(ctx.store.parseStorePath(actualPath)));
        // debug("getDetailedCycleError: hostPath2=%s", hostPath2);

        auto accessor = makeFSSourceAccessor(hostPath);

        // remove the chroot prefix path before "/nix/store/"
        // TODO better?
        size_t chrootPrefixLen = hostPath.size() - actualPath.size();
        std::string chrootPrefix = hostPath.substr(0, chrootPrefixLen);

        scanForCycleEdges(
            ctx.store,
            *accessor,
            chrootPrefixLen,
            chrootPrefix,
            CanonPath("/"),
            ctx.referenceablePaths,
            edges
        );
    }

    if (edges.empty()) {
        debug("no detailed cycle edges found, rethrowing");
        return ctx.error;
    }

    debug("found %lu cycle edges, transforming to connected paths", edges.size());

    // Transform individual edges into connected multi-edges (paths)
    StoreCycleEdgeVec multiedges;
    transformEdgesToMultiedges(edges, multiedges);

    // Build detailed error message
    // ANSI_NORMAL because i hate pink
    std::string pathsStr = multiedges.size() == 1 ? "path" : "paths";
    std::string cycleDetails = fmt(ANSI_NORMAL "Found %d cycle %s:", multiedges.size(), pathsStr);

    for (size_t i = 0; i < multiedges.size(); i++) {
        auto & multiedge = multiedges[i];
        cycleDetails += fmt("\n\n%d:", i + 1);
        for (auto & file : multiedge) {
            cycleDetails += fmt("\n  - %s", file);
        }
    }

    // yeah i know what a cycle is...
    // and everyone else can google:
    // nix error: cycle detected in build
    // cycleDetails +=
    //     "\n\nThis means there are circular references between output files.\n"
    //     "The build cannot proceed because the outputs reference each other.";

    // Add hint with temp paths for debugging
    if (settings.keepFailed || verbosity >= lvlDebug) {
        cycleDetails +=
            fmt("\n\nNote: Build outputs are kept for inspection.\n"
                "You can examine the files listed above to understand the cycle.");
    }

    // Throw new error with original message + cycle details
    std::string originalMsg;
    try {
        auto &be = dynamic_cast<const BuildError &>(ctx.error);
        originalMsg = be.msg();
    } catch (...) {
        originalMsg = ctx.error.what();
    }

    // dont duplicate the "error: " prefix
    for (auto prefix : {
            ANSI_RED "error:" ANSI_NORMAL " ",
            "error: "
        })
    {
        if (originalMsg.starts_with(prefix)) {
            originalMsg.erase(0, strlen(prefix));
            break;
        }
    }

    // ANSI_NORMAL because i hate pink
    originalMsg = ANSI_NORMAL + originalMsg;

    // repeat the error message
    // so users dont have to scroll up
    // this may be useful if we find many cycles
    cycleDetails += "\n\n" ANSI_RED "error:" ANSI_NORMAL " " + originalMsg;

    return BuildError(
        BuildResult::Failure::OutputRejected,
        "%s\n\n%s",
        originalMsg,
        cycleDetails
    );
}

} // namespace nix
