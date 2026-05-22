#include "nix/store/build/find-cycles.hh"

// FIXME also find references to hash or hash-name
// a reference to a derivation hash alone is enough to form a cycle edge
// if we only find hash or hash-name
// then we have to "to" path
// so that is the end of a cycle

// no, this is too complex for now
// this would require to turn the StoreCycleEdge type into a struct
// where we can also store the raw match string
// but that extra complexity is usually not worth it
// because users can just search for the derivation hash instead of the derivation path
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

// TODO also find concatted store paths like
// $out$out/some/path
// $out$dev/some/path
// or is this out of scope?

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
    const StorePathSet & refs, // otherOutputsStorePathSet
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
    SourceAccessor & fromDrvAccessor,
    size_t chrootPrefixLen,
    const std::string chrootPrefix,
    // fromFileRelPath = "/path/to/file"
    const CanonPath & fromFileRelPath,
    const std::map<std::string, StorePath> & hashPathMap,
    StoreCycleEdgeVec & cycleEdges
)
{
    // debug("scanForCycleEdges2: fromFileRelPath=%s", fromFileRelPath);

    auto fromFileInfo = fromDrvAccessor.lstat(fromFileRelPath);

    // read file
    std::string fromFileContent;
    if (fromFileInfo.type == SourceAccessor::tSymlink) {
        fromFileContent = fromDrvAccessor.readLink(fromFileRelPath);
        // content can be anything, so we escape it to a JSON string
        debug("scanForCycleEdges2: fromFileRelPath is a symlink: %s -> fromFileContent=%s", fromFileRelPath, nlohmann::json(fromFileContent).dump());
    } else if (fromFileInfo.type == SourceAccessor::tRegular) {
        debug("scanForCycleEdges2: fromFileRelPath is a file: %s", fromFileRelPath);
        // TODO read file in chunks
        // otherwise with large files, we can run out of memory
        auto _fromFileContent = fromDrvAccessor.readFile(fromFileRelPath);
        // fromFileContent can be empty, but we can still find a match in fromFilePathStr
        fromFileContent = _fromFileContent;
    } else if (fromFileInfo.type == SourceAccessor::tDirectory) {
        debug("scanForCycleEdges2: fromFileRelPath is a directory: %s", fromFileRelPath);
        // fromFileContent is empty, but we can still find a match in fromFilePathStr
    } else {
        debug("scanForCycleEdges2: fromFileRelPath is of unknown type: %s", fromFileRelPath);
        return;
    }

    // fromFilePathStr = "/nix/store/hash-name/path/to/file"
    auto fromFilePathStr = fromDrvAccessor.showPath(fromFileRelPath);

    // remove the chroot prefix path before "/nix/store/"
    // TODO better?
    if (chrootPrefixLen > 0 && fromFilePathStr.size() >= chrootPrefixLen)
        fromFilePathStr.erase(0, chrootPrefixLen);

    // fromDirPath = "/nix/store/hash-name/path/to"
    std::filesystem::path fromDirPath = std::filesystem::path(fromFilePathStr).parent_path();

    // src/libstore/include/nix/store/path.hh
    // StorePath.HashLen
    constexpr size_t hashLen = 32;

    auto isPathByte = [](unsigned char c) {
        return c != '\0' &&
            c != '"'  &&
            c != '\'' &&
            c != '<'  &&
            c != '>'  &&
            c != '|'  &&
            c != '\r' &&
            c != '\n';
    };

    // loop target paths
    // NOTE this could be optimized by searching multiple hashes in parallel
    // but this code almost never runs, so dont optimize
    for (const auto & [toDrvHash, toDrvStorePath] : hashPathMap) {

        // toDrvPathStr = "/nix/store/hash-name"
        const std::string toDrvPathStr = store.printStorePath(toDrvStorePath);
        // debug("scanForCycleEdges2: toDrvPathStr=%s", toDrvPathStr);

        // toDrvBasename = "hash-name"
        const auto toDrvBasename = std::string(store.parseStorePath(toDrvPathStr).to_string());
        // debug("scanForCycleEdges2: toDrvPathStr=%s toDrvBasename=%s", toDrvPathStr, toDrvBasename);

        // toDrvBasenameSuffix = "-name"
        const std::string toDrvBasenameSuffix = toDrvBasename.substr(hashLen);

        std::optional<ref<SourceAccessor>> toDrvAccessor;
        try {

            // error: ‘realPathInHost’ was not declared in this scope
            // virtual std::filesystem::path realPathInHost(const std::filesystem::path & p)
            // {
            //     return store.toRealPath(store.parseStorePath(p.native()));
            // }
            // toDrvAccessor = makeFSSourceAccessor(realPathInHost(toDrvPathStr));

            // FIXME failed to get toDrvAccessor: error: opening file "/nix/store/l6jk0s32idk8pdr5wi0kzj19blvkiyky-cyclic-outputs-dev": No such file or directory
            // quickfix: add chrootPrefix
            // why does makeFSSourceAccessor fail to add chrootPrefix?
            toDrvAccessor = makeFSSourceAccessor(
                // store.toRealPath(store.parseStorePath(toDrvPathStr))
                chrootPrefix + std::string(store.toRealPath(store.parseStorePath(toDrvPathStr)))
            );
        }
        catch (std::exception & e) {
            debug("failed to get toDrvAccessor: %s", e.what());
            throw;
        }

        // search in fromFilePathStr and content
        for (std::string fromData : {fromFilePathStr, fromFileContent}) {

            const size_t fromDataSize = fromData.size();

            size_t toDrvHashPos = 0;
            size_t toFilePathMatchEnd = 0;

            // loop toDrvHash matches
            while (true) {

                // search for toDrvHash
                // start searching at toDrvHashPos
                toDrvHashPos = fromData.find(toDrvHash, toDrvHashPos);
                if (toDrvHashPos == std::string::npos) {
                    // end of file
                    break;
                }

                // found toDrvHash
                std::string toFilePathStr = "/nix/store/" + toDrvHash;
                const size_t toDrvHashEnd = toDrvHashPos + hashLen;
                toFilePathMatchEnd = toDrvHashEnd;
                debug(
                    "scanForCycleEdges2: found toDrvHash=%s at toDrvHashPos=%zu in fromFilePathStr=%s",
                    toDrvHash,
                    toDrvHashPos,
                    fromFilePathStr
                );

                // search for toDrvBasename
                bool toDrvBasenameFound = true;
                for (size_t toSuffixIdx = 0; toSuffixIdx < toDrvBasenameSuffix.size(); ++toSuffixIdx) {
                    size_t fromDataIdx = toFilePathMatchEnd + toSuffixIdx;
                    if (fromDataIdx >= fromDataSize) {
                        toDrvBasenameFound = false;
                        break;
                    }
                    if (fromData[fromDataIdx] != toDrvBasenameSuffix[toSuffixIdx]) {
                        toDrvBasenameFound = false;
                        break;
                    }
                    // partial match of toDrvBasename
                }

                if (!toDrvBasenameFound) {
                    // found toDrvHash only
                    debug("found toDrvHash only: toFilePathStr=%s", nlohmann::json(toFilePathStr).dump());
                    cycleEdges.push_back(StoreCycleEdge{fromFilePathStr, toFilePathStr});

                    // continue searching after this match
                    // toDrvHashPos += hashLen;
                    toDrvHashPos = toFilePathMatchEnd;

                    continue;
                }

                // found toDrvBasename
                const size_t toDrvBasenameEnd = toDrvHashEnd + toDrvBasenameSuffix.size();
                toFilePathMatchEnd = toDrvBasenameEnd;
                // prepend store dir
                toFilePathStr = "/nix/store/" + toDrvBasename;

                // search for toFileRelPathStr
                std::optional<std::string> toFileRelPathStrOpt;

                // seek into fromData, byte by byte
                // TODO off by one?
                const size_t toFileRelPathStart = toDrvBasenameEnd;

                // debug("toFileRelPathStart=%d", toFileRelPathStart);
                // debug("fromData[toFileRelPathStart]=%s", nlohmann::json(std::string(1, fromData[toFileRelPathStart])).dump());

                // relative path must start with '/'
                if (toFileRelPathStart >= fromDataSize || fromData[toFileRelPathStart] != '/') {
                    // found toDrvBasename only
                    debug("not found relative path start. found toDrvBasename only: toFilePathStr=%s", nlohmann::json(toFilePathStr).dump());
                    cycleEdges.push_back(StoreCycleEdge{fromFilePathStr, toFilePathStr});

                    // continue searching after this match
                    toDrvHashPos = toDrvBasenameEnd;
                    continue;
                }

                for (
                    // TODO off by one?
                    // TODO off by two? (we have already consumed the leading '/')
                    // size_t toFileRelPathEnd = toFileRelPathStart;
                    size_t toFileRelPathEnd = toFileRelPathStart + 1;
                    toFileRelPathEnd < fromDataSize;
                    ++toFileRelPathEnd
                )
                {
                    // debug("toFileRelPathEnd=%d fromData[toFileRelPathEnd]=%s", toFileRelPathEnd, nlohmann::json(std::string(1, fromData[toFileRelPathEnd])).dump());

                    // TODO verify
                    if (!isPathByte(fromData[toFileRelPathEnd])) break;

                    // TODO off by one?
                    // const size_t toFileRelPathLen = toFileRelPathEnd - toDrvHashPos + 1;
                    // std::string toFileRelPathRawStr = fromData.substr(toDrvHashPos, toFileRelPathLen);
                    const size_t toFileRelPathLen = toFileRelPathEnd - toFileRelPathStart + 1;
                    std::string toFileRelPathRawStr = fromData.substr(toFileRelPathStart, toFileRelPathLen);
                    assert(!toFileRelPathRawStr.empty());
                    // debug("toFileRelPathRawStr=%s", nlohmann::json(toFileRelPathRawStr).dump());

                    // at this point, there is no need to resolve relative paths
                    // since we have already found toDrvBasename
                    // so we already are at "/nix/store/hash-name"

                    // no! weakly_canonical has filesystem access
                    // std::string normalized = std::filesystem::weakly_canonical(joined).string();

                    // lexical normalization without filesystem access
                    // candidate for toFileRelPathStr
                    std::string toFileRelPathStrCand = std::filesystem::path(toFileRelPathRawStr).lexically_normal();
                    // debug("toFileRelPathStrCand=%s", nlohmann::json(toFileRelPathStrCand).dump());

                    // Existence check
                    if ((**toDrvAccessor).pathExists(CanonPath(toFileRelPathStrCand))) {
                        toFileRelPathStrOpt = toFileRelPathStrCand;
                        toFilePathMatchEnd = toFileRelPathEnd;
                    }
                }

                if (!toFileRelPathStrOpt) {
                    // found toDrvBasename only
                    debug("not found toFileRelPathStr. found toDrvBasename only: toFilePathStr=%s", nlohmann::json(toFilePathStr).dump());
                    cycleEdges.push_back(StoreCycleEdge{fromFilePathStr, toFilePathStr});

                    // continue searching after this match
                    toDrvHashPos = toDrvBasenameEnd;
                    continue;
                }

                // found toFileRelPathStr
                // std::string toFilePathStr = "/nix/store/" + toDrvBasename + *toFileRelPathStrOpt;
                const std::string toFileRelPathStr = *toFileRelPathStrOpt;
                toFilePathStr = "/nix/store/" + toDrvBasename + toFileRelPathStr;
                debug("found relative path: toFilePathStr=%s", nlohmann::json(toFilePathStr).dump());
                cycleEdges.push_back(StoreCycleEdge{fromFilePathStr, toFilePathStr});

                // continue searching after this match
                toDrvHashPos = toFilePathMatchEnd;
            }
        }
    }

    if (fromFileInfo.type != SourceAccessor::tDirectory) {
        return;
    }

    // recursion
    // debug("scanForCycleEdges2: fromFileRelPath is a directory: %s", fromFileRelPath);
    for (const auto & [name, type] : fromDrvAccessor.readDirectory(fromFileRelPath)) {
        scanForCycleEdges2(
            store,
            fromDrvAccessor,
            chrootPrefixLen,
            chrootPrefix,
            fromFileRelPath / name,
            hashPathMap,
            cycleEdges);
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

    const auto & scanOutputsResult = ctx.scanOutputs();

    for (const auto & outputItem : scanOutputsResult) {

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

        StorePathSet otherOutputsStorePathSet;
        for (const auto & outputItem : scanOutputsResult) {
            std::string otherActualPath = outputItem[1];
            if (otherActualPath == actualPath) {
                // dont search for self-references in actualPath
                continue;
            }
            if (!otherActualPath.starts_with("/nix/store/")) {
                throw Error(fmt("getDetailedCycleError: bad otherActualPath: %s", nlohmann::json(otherActualPath).dump()));
            }
            std::string otherBaseName = otherActualPath.substr(strlen("/nix/store/"));
            // debug("otherActualPath=%s otherBaseName=%s", otherActualPath, otherBaseName);
            StorePath otherActualStorePath(otherBaseName);
            otherOutputsStorePathSet.insert(otherActualStorePath);
        }

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
            otherOutputsStorePathSet,
            edges
        );
    }

    if (edges.empty()) {
        debug("no detailed cycle edges found, rethrowing");
        // TODO modify the error message?
        return ctx.error;
    }

    debug("found %lu cycle edges, transforming to connected paths", edges.size());

    // Transform individual edges into connected multi-edges (paths)
    StoreCycleEdgeVec multiedges;
    transformEdgesToMultiedges(edges, multiedges);

    // Build detailed error message
    // ANSI_NORMAL because i hate pink
    std::string edgesStr = multiedges.size() == 1 ? "edge" : "edges";
    std::string cycleDetails = fmt(ANSI_NORMAL "Found %d cycle %s:", multiedges.size(), edgesStr);

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
    // "nix-build --keep-failed" also says:
    // note: keeping build directory "/nix/var/nix/builds/nix-1111111-222222222/build"
    // but here we care about the build outputs
    if (settings.keepFailed || verbosity >= lvlDebug) {
        cycleDetails +=
            // fmt("\n\nNote: Build outputs are kept for inspection.\n"
            //     "You can examine the files listed above to understand the cycle.");
            // fmt("\n\nNote: The build outputs are kept in the Nix store for manual inspection.");
            fmt("\n\nNote: The build outputs are kept for manual inspection.");
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
