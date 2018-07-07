/*

 Ok folks, what I think I want is ...
 to compute for the whole file and then cache that.
 Also that way I can better handle definitions

 What will come out of this?
 A mapping of stamp -> (location, type, option(docs))
 And toplevelname -> stamp
 andddd maybe that's it?
 Oh right, a list of [loc, type, path] for the hover bit
 and probably a happing of stamp -> list(loc) of references

 umm I also want open mapping

 also thinking about providing rename functionality, and "find references"


 err what about stamps that are modules?
 maybe have a separate map for that?

 */
open Infix;

type item =
  | Module(list((string, int)))
  | ModuleWithDocs(list(Docs.full))
  /* | ModuleAlias(Path.t) */
  | Type(Types.type_declaration)
  | Constructor(Types.constructor_declaration, string, Types.type_declaration)
  | Attribute(Types.type_expr, string, Types.type_declaration)
  | Value(Types.type_expr);

type definition =
  | Path(Path.t)
  | Open(Path.t)
  /* | Location(Location.t) */
  | ConstructorDefn(Path.t, string, Location.t)
  | AttributeDefn(Path.t, string, Location.t)
  | IsConstant
  | IsDefinition(int);

type tag =
  | TagType
  | TagValue
  | TagModule
  | TagConstructor(string)
  | TagAttribute(string);

type anOpen = {
  path: Path.t,
  loc: Location.t,
  mutable used: list((Longident.t, tag, Location.t)),
  mutable useCount: int
};

type moduleData = {
  mutable toplevelDocs: option(string),
  stamps: Hashtbl.t(int, (string, Location.t, item, option(string), ((int, int), (int, int)))),
  internalReferences: Hashtbl.t(int, list(Location.t)),
  externalReferences: Hashtbl.t(string, list((list(string), Location.t, option(string)))),
  exported: Hashtbl.t(string, int),
  mutable exportedSuffixes: list((int, string, string)),
  mutable topLevel: list((string, int)),
  mutable locations: list((Location.t, Types.type_expr, definition)),
  mutable explanations: list((Location.t, string)),
  mutable allOpens: list(anOpen)
};

let maybeFound = (fn, a) =>
  switch (fn(a)) {
  | exception Not_found => None
  | x => Some(x)
  };

let rec docsItem = (item, data) =>
  switch item {
  | Type(t) => Docs.Type(t)
  | ModuleWithDocs(docs) => Docs.Module(docs)
  | Constructor(a, b, c) => Docs.Constructor(a, b, c)
  | Attribute(a, b, c) => Docs.Attribute(a, b, c)
  | Value(t) => Docs.Value(t)
  | Module(items) =>
    Docs.Module(
      items
      |> List.map(
           ((name, stamp)) => {
             let (name, loc, item, docs, _) = Hashtbl.find(data.stamps, stamp);
             (name, loc, docs, docsItem(item, data))
           }
         )
    )
  };

let inRange = ((l, c), ((l0, c0), (l1, c1))) => {
  let l = l + 1;
  (l0 < l || l0 == l && c0 <= c) && (l1 == (-1) && c1 == (-1) || l1 > l || l1 == l && c1 > c)
};

let rec dig = (typ) =>
  switch typ.Types.desc {
  | Types.Tlink(inner) => dig(inner)
  | Types.Tsubst(inner) => dig(inner)
  | _ => typ
  };

let getSuffix = (declaration, suffix) =>
  switch declaration.Types.type_kind {
  | Type_record(attributes, _) =>
    Utils.find(
      ({Types.ld_id: {name, stamp}, ld_loc}) =>
        if (name == suffix) {
          Some((ld_loc, stamp))
        } else {
          None
        },
      attributes
    )
  | Type_variant(constructors) =>
    Utils.find(
      ({Types.cd_id: {name, stamp}, cd_loc}) =>
        if (name == suffix) {
          Some((cd_loc, stamp))
        } else {
          None
        },
      constructors
    )
  | _ => None
  };

let suffixForStamp = (stamp, suffix, data) => {
  let%opt (name, loc, item, docs, range) = maybeFound(Hashtbl.find(data.stamps), stamp);
  switch item {
    | Type(t) => getSuffix(t, suffix) |?>> ((loc, stamp)) => stamp
    | _ => None
  }
};

let rec stampAtPath = (path, data, suffix) =>
  switch path {
  | Path.Pident({stamp: 0, name}) => Some(`Global((name, [], suffix)))
  | Path.Pident({stamp, name}) => fold(suffix, Some(`Local(stamp)), suffix => suffixForStamp(stamp, suffix, data) |?>> stamp => `Local(stamp))
  | Path.Pdot(inner, name, _) =>
    switch (stampAtPath(inner, data, None)) {
    | Some(`Global(top, subs, _)) => Some(`Global((top, subs @ [name], suffix)))
    | Some(`Local(stamp)) =>
      let%opt x = maybeFound(Hashtbl.find(data.stamps), stamp);
      switch x {
      | (_, _, Module(contents), _, _) =>
        maybeFound(List.assoc(name), contents) |?>> ((stamp) => `Local(stamp))
      | _ => None
      }
    | _ => None
    }
  | _ => None
  };

/* TODO this is not perfect, because if the user edits and gets outside of the original scope, then
   we no longer give you the completions you need. This is annoying :/
   Not sure how annoying in practice? One hack would be to forgive going a few lines over... */
let completions = ({stamps}, prefix, pos) => {
  Hashtbl.fold(
    (_, (name, loc, item, docs, range), results) =>
      if (inRange(pos, range)) {
        if (Utils.startsWith(name, prefix)) {
          [(name, loc, item, docs), ...results]
        } else {
          results
        };
      } else { results },
    stamps,
    []
  )
};

let resolvePath = (path, data, suffix) =>
  switch (stampAtPath(path, data, suffix)) {
  | None => None
  | Some(`Global(name, children, suffix)) => Some(`Global((name, children, suffix)))
  | Some(`Local(stamp)) => maybeFound(Hashtbl.find(data.stamps), stamp) |?>> i => `Local(i)
  };

let findDefinition = (defn, data, resolve) => {
  /* Log.log("😍 resolving a definition"); */
  switch defn {
  | IsConstant => None
  | IsDefinition(stamp) =>
    Log.log("Is a definition");
    None
  | ConstructorDefn(path, name, _) => resolvePath(path, data, Some(name)) |?> resolve
  | AttributeDefn(path, name, _) => resolvePath(path, data, Some(name)) |?> resolve
  | Open(path)
  | Path(path) => resolvePath(path, data, None) |?> resolve
  };
};

let completionPath = (inDocs, {stamps} as moduleData, first, children, pos, toItem, ~resolveDefinition) => {
  let%opt_wrap (name, loc, item, docs) = Hashtbl.fold(
    (_, (name, loc, item, docs, range), result) =>
      switch result {
      | Some(x) => Some(x)
      | None => {
        if (inRange(pos, range) && name == first) {
          Some((name, loc, item, docs))
        } else {
          None
        }
      }
    },
    stamps,
    None
  );

  switch item {
    | ModuleWithDocs(docs) => inDocs(children, docs)
    | Module(contents) => {
      let rec loop = (contents, items) => {
        switch (items) {
          | [] => assert(false)
          | [single] => {
            contents
            |> List.filter(((name, stamp)) => Utils.startsWith(name, single))
            |> List.map(((name, stamp)) => {
              toItem(Hashtbl.find(stamps, stamp))
            })
          }
          | [first, ...more] => {
            switch (List.find(((name, stamp)) => name == first, contents)) {
              | (name, stamp) => {
                let (name, loc, item, docs, range) = Hashtbl.find(stamps, stamp);
                switch item {
                  | Module(contents) => loop(contents, more)
                  | ModuleWithDocs(docs) => inDocs(more, docs)
                  | _ => []
                }
              }
              | exception Not_found => []
            }
          }
        }
      };
      loop(contents, children)
    }
    | Value(t) => {
      let rec loop = (t, children) => {
        switch (dig(t).Types.desc) {
          | Types.Tconstr(path, args, _abbrev) => {
            let%opt stamp = stampAtPath(path, moduleData, None);
            let%opt (item, loc, _, _) = findDefinition(Path(path), moduleData, resolveDefinition);
            switch item {
              | Docs.Type({type_kind: Type_record(labels, _)} as declaration) => {
                switch children {
                  | [] => None
                  | [single] => labels |> List.filter(({Types.ld_id: {name}}) => Utils.startsWith(name, single))
                  |> List.map(({Types.ld_id: {name}, ld_type, ld_loc}) => toItem((name, ld_loc, Attribute(
                    ld_type, 
                    name,
                    declaration
                  ), None, ((0,0),(0,0))))) |. Some
                  | [first, ...rest] => {
                    labels |> Utils.find(({Types.ld_id: {name}, ld_type, ld_loc}) => {
                      if (name == first) {
                        loop(ld_type, rest)
                      } else {
                        None
                      }
                    })
                  }
                }
              }
              /* This really should never happen */
              | _ => None
            }
          }
          | _ => None
        };
      };
      loop(t, children) |? []
    }
    | _ => []
  }
};

module Opens = {
  let pushHashList = (hash, key, value) =>
    Hashtbl.replace(
      hash,
      key,
      switch (Hashtbl.find(hash, key)) {
      | exception Not_found => [value]
      | items => [value, ...items]
      }
    );
  let mapHash = (hash, fn) => Hashtbl.fold((k, v, l) => [fn(k, v), ...l], hash, []);
  let toString = (fn, (a, tag)) =>
    switch tag {
    | TagType => "type: " ++ fn(a)
    | TagValue => "value: " ++ fn(a)
    | TagConstructor(b) => "constr: " ++ fn(a) ++ " - " ++ b
    | TagAttribute(b) => "attr: " ++ fn(a) ++ " - " ++ b
    | TagModule => "module: " ++ fn(a)
    };
  let showLident = (l) => String.concat(".", Longident.flatten(l));
  let rec addLidentToPath = (path, lident) =>
    Path.(
      Longident.(
        switch lident {
        | Lident(text) => Pdot(path, text, 0)
        | Ldot(lident, text) => Pdot(addLidentToPath(path, lident), text, 0)
        | Lapply(_, _) => failwith("I dont know what these are")
        }
      )
    );
  let showUses = (openPath, uses) => {
    let attrs = Hashtbl.create(50);
    let constrs = Hashtbl.create(50);
    let normals =
      List.filter(
        ((innerPath, tag)) =>
          switch tag {
          | TagConstructor(name) =>
            pushHashList(constrs, innerPath, name);
            false
          | TagAttribute(name) =>
            pushHashList(attrs, innerPath, name);
            false
          | _ => true
          },
        uses
      );
    let normals =
      normals
      |> List.filter(
           ((innerPath, tag)) =>
             switch tag {
             | TagType => ! (Hashtbl.mem(attrs, innerPath) || Hashtbl.mem(constrs, innerPath))
             | _ => true
             }
         );
    List.concat([
      normals
      |> List.map(
           ((lident, tag)) => {
             let fullPath = addLidentToPath(openPath, lident);
             showLident(lident)
           }
         ),
      mapHash(
        attrs,
        (path, attrs) => {
          let fullPath = addLidentToPath(openPath, path);
          showLident(path) ++ " {" ++ String.concat(", ", attrs |> List.map((attr) => attr)) ++ "}"
        }
      ),
      mapHash(
        constrs,
        (path, attrs) => {
          let fullPath = addLidentToPath(openPath, path);
          showLident(path)
          ++ " ("
          ++ String.concat(" | ", attrs |> List.map((attr) => attr))
          ++ ")"
        }
      )
    ])
    |> String.concat(", ")
  };
};

let opens = ({allOpens}) =>
  allOpens
  |> Utils.filterMap(
       ({path, loc, used, useCount}) =>
         if (! loc.Location.loc_ghost) {
           let i = loc.Location.loc_end.pos_cnum;
           let isPervasives =
             switch path {
             | Path.Pident({name: "Pervasives"}) => true
             | _ => false
             };
           let used = List.sort_uniq(compare, List.map(((ident, tag, _)) => (ident, tag), used));
           Some((
             "exposing ("
             ++ Opens.showUses(path, used)
             ++ ") "
             ++ string_of_int(useCount)
             ++ " uses",
             loc
           ))
         } else {
           None
         }
     );

let dependencyList = ({externalReferences}) =>
  Hashtbl.fold((k, _, items) => [k, ...items], externalReferences, []);

let listExported = (data) =>
  Hashtbl.fold(
    (name, stamp, results) =>
      switch (Hashtbl.find(data.stamps, stamp)) {
      | exception Not_found => results
      | item => [item, ...results]
      },
    data.exported,
    []
  );

let listTopLevel = (data) =>
  data.topLevel |> List.map(((name, stamp)) => Hashtbl.find(data.stamps, stamp));

let handleConstructor = (path, txt) => {
  let typeName =
    switch path {
    | Path.Pdot(path, typename, _) => typename
    | Pident({Ident.name}) => name
    | _ => assert false
    };
  Longident.(
    switch txt {
    | Longident.Lident(name) => (name, Lident(typeName))
    | Ldot(left, name) => (name, Ldot(left, typeName))
    | Lapply(_) => assert false
    }
  )
};

let handleRecord = (path, txt) => {
  let typeName =
    switch path {
    | Path.Pdot(path, typename, _) => typename
    | Pident({Ident.name}) => name
    | _ => assert false
    };
  Longident.(
    switch txt {
    | Lident(name) => Lident(typeName)
    | Ldot(inner, name) => Ldot(inner, typeName)
    | Lapply(_) => assert false
    }
  )
};

let resolveNamedPath = (data, path, suffix) =>
  switch path {
  | [] => None
  | [one, ...rest] =>
    switch (Hashtbl.find(data.exported, one)) {
    | exception Not_found => None
    | stamp =>
      let rec loop = (stamp, path) =>
        switch (Hashtbl.find(data.stamps, stamp)) {
        | exception Not_found => None
        | (name, loc, item, docs, scope) =>
          switch (path, item) {
          | ([], _) => switch (item, suffix) {
            | (_, None) => Some((name, loc, item, docs))
            | (Type(t), Some(suffix)) => {
              /* TODO this isn't the right `item` --- it'sstill the type  */
              let%opt_wrap (loc, _) = getSuffix(t, suffix);
              (name, loc, item, docs)
            }
            | _ => None
          }
          | ([first, ...rest], Module(contents)) =>
            switch (List.assoc(first, contents)) {
            | exception Not_found => None
            | stamp => loop(stamp, rest)
            }
          | _ => None
          }
        };
      loop(stamp, rest)
    }
  };

let checkPos = ((line, char), {Location.loc_start: {pos_lnum, pos_bol, pos_cnum}, loc_end}) =>
  Lexing.(
    if (line < pos_lnum || line == pos_lnum && char < pos_cnum - pos_bol) {
      false
    } else if (line > loc_end.pos_lnum
               || line == loc_end.pos_lnum
               && char > loc_end.pos_cnum
               - loc_end.pos_bol) {
      false
    } else {
      true
    }
  );

let explanationAtPos = ((line, char), data) => {
  let pos = (line + 1, char);
  let rec loop = (locations) =>
    switch locations {
    | [] => None
    | [(loc, explanation), ..._] when checkPos(pos, loc) => Some((loc, explanation))
    | [_, ...rest] => loop(rest)
    };
  loop(data.explanations)
};

let locationAtPos = ((line, char), data) => {
  let pos = (line + 1, char);
  let rec loop = (locations) =>
    switch locations {
    | [] => None
    | [(loc, expr, defn), ..._] when checkPos(pos, loc) => Some((loc, expr, defn))
    | [_, ...rest] => loop(rest)
    };
  loop(data.locations)
};

let openReferencesAtPos = ({allOpens} as data, pos) => {
  let%opt (loc, _expr, defn) = locationAtPos(pos, data);
  switch defn {
  | Open(path) => {
    let rec loop = opens => switch opens {
      | [] => None
      | [one, ..._] when one.loc == loc => Some(one)
      | [_, ...rest] => loop(rest)
    };
    let%opt openn = loop(allOpens);
    Some(openn.used)
  }
  | _ => None
  }
};

let isStampExported = (needle, data) =>
  switch (Hashtbl.fold(
    (name, stamp, found) => found != None ? found : stamp == needle ? Some((name, None)) : None,
    data.exported,
    None
  )) {
    | Some(m) => Some(m)
    | None => data.exportedSuffixes |> Utils.find(((suffixStamp, mainName, suffixName)) => suffixStamp == needle ? Some((mainName, Some(suffixName))) : None)
  };

let highlightsForStamp = (stamp, data) =>{
  let%opt (name, defnLoc, _,_, _) = maybeFound(Hashtbl.find(data.stamps), stamp);
  let usages = maybeFound(Hashtbl.find(data.internalReferences), stamp) |? [];
  Some([(`Write, defnLoc), ...List.map((l) => (`Read, Utils.endOfLocation(l, String.length(name))), usages)])
};

let stampAtPos = (pos, data) => {
  let%opt (loc, expr, defn) = locationAtPos(pos, data);
  switch defn {
  | IsDefinition(stamp) => Some(stamp)
  | AttributeDefn(path, name, _) =>
    switch (stampAtPath(path, data, Some(name))) {
    | Some(`Global(name, children, _)) => None /* TODO resolve cross-file */
    | Some(`Local(stamp)) => Some(stamp)
    | None => None
    }
  | ConstructorDefn(path, name, _) =>
    switch (stampAtPath(path, data, Some(name))) {
    | Some(`Global(name, children, _)) => None /* TODO resolve cross-file */
    | Some(`Local(stamp)) => Some(stamp)
    | None => None
    }
  | Path(path) =>
    switch (stampAtPath(path, data, None)) {
    | Some(`Global(name, children, _)) => None /* TODO resolve cross-file */
    | Some(`Local(stamp)) => Some(stamp)
    | None => None
    }
  | _ => None
  }
};

let highlights = (pos, data) => stampAtPos(pos, data) |?> ((x) => highlightsForStamp(x, data));

let locationSize = ({Location.loc_start, loc_end}) => loc_end.Lexing.pos_cnum - loc_start.Lexing.pos_cnum;

module Get = {
  /* For a list of structure items, get the names and stamps of definted things in there.
   */
  let rec stampNames = (items) =>
    Typedtree.(
      items
      |> List.map(
           (item) =>
             switch item.str_desc {
             | Tstr_value(_, bindings) =>
               bindings
               |> PrepareUtils.filterNil(
                    (binding) =>
                      switch binding {
                      | {vb_pat: {pat_desc: Tpat_var({stamp, name}, _)}} => Some((name, stamp))
                      | _ => None
                      }
                  )
             | Tstr_type(decls) => decls |> List.map(({typ_id: {stamp, name}}) => (name, stamp))
             | Tstr_module({mb_id: {stamp, name}}) => [(name, stamp)]
             | Tstr_modtype({mtd_id: {stamp, name}}) => [(name, stamp)]
             /* | Tstr_include({incl_type}) */
             | _ => []
             }
         )
      |> List.concat
    );
  module F = (Collector: {let data: moduleData; let allOpens: ref(list(anOpen));}) => {
    open Typedtree;
    include TypedtreeIter.DefaultIteratorArgument;
    let posOfLexing = ({Lexing.pos_lnum, pos_cnum, pos_bol}) => (pos_lnum, pos_cnum - pos_bol);
    let rangeOfLoc = ({Location.loc_start, loc_end}) => (
      posOfLexing(loc_start),
      posOfLexing(loc_end)
    );
    let openScopes = ref([ref([])]);
    let addOpenScope = () => openScopes := [ref([]), ...openScopes^];
    let popOpenScope = () => openScopes := List.tl(openScopes^);
    let addOpen = (path, loc) => {
      let top = List.hd(openScopes^);
      let op = {path, loc, used: [], useCount: 0};
      top := [op, ...top^];
      Collector.allOpens := [op, ...Collector.allOpens^]
    };
    let rec usesOpen = (ident, path) =>
      switch (ident, path) {
      | (Longident.Lident(name), Path.Pdot(path, pname, _)) => true
      | (Longident.Lident(_), Path.Pident(_)) => false
      | (Longident.Ldot(ident, _), Path.Pdot(path, _, _)) => usesOpen(ident, path)
      | (Ldot(_), Pident({name: "*predef*" | "exn"})) => false
      | (Ldot(Lident("*predef*" | "exn"), _), Pident(_)) => false
      | _ =>
        failwith(
          "Cannot open " ++ Path.name(path) ++ " " ++ String.concat(".", Longident.flatten(ident))
        )
      };
    let rec relative = (ident, path) =>
      switch (ident, path) {
      | (Longident.Lident(name), Path.Pdot(path, pname, _)) when pname == name => path
      | (Longident.Ldot(ident, _), Path.Pdot(path, _, _)) => relative(ident, path)
      | (Ldot(Lident("*predef*" | "exn"), _), Pident(_)) => path
      | _ =>
        failwith(
          "Cannot relative "
          ++ Path.name(path)
          ++ " "
          ++ String.concat(".", Longident.flatten(ident))
        )
      };
    let addUse = ((path, tag), ident, loc) => {
      let openNeedle = relative(ident, path);
      let rec loop = (stacks) =>
        switch stacks {
        | [] => ()
        | [stack, ...rest] =>
          let rec inner = (opens) =>
            switch opens {
            | [] => loop(rest)
            | [{path} as one, ...rest] when Path.same(path, openNeedle) =>
              one.used = [(ident, tag, loc), ...one.used];
              one.useCount = one.useCount + 1
            | [{path}, ...rest] => inner(rest)
            };
          inner(stack^)
        };
      loop(openScopes^)
    };
    let scopes = ref([((0, 0), ((-1), (-1)))]);
    let addScope = (loc) => scopes := [loc, ...scopes^];
    let popScope = () =>
      scopes :=
        (
          switch scopes^ {
          | [] => []
          | [_, ...rest] => rest
          }
        );
    let currentScope = () => List.hd(scopes^);
    let addStamp = (stamp, name, loc, item, docs) =>
      if (! Hashtbl.mem(Collector.data.stamps, stamp)) {
        Hashtbl.replace(Collector.data.stamps, stamp, (name, loc, item, docs, currentScope()))
      };

    let addLocation = (loc, typ, definition) => {
      switch definition {
      | ConstructorDefn(path, name, _) => Some((path, Some(name)))
      | AttributeDefn(path, name, _) => Some((path, Some(name)))
      | Path(path) => Some((path, None))
      | _ => None
      } |?< ((path, suffix)) =>
        switch (stampAtPath(path, Collector.data, suffix)) {
        | None => ()
        | Some(`Global(modname, children, suffix)) =>
          let current = maybeFound(Hashtbl.find(Collector.data.externalReferences), modname) |? [];
          Hashtbl.replace(
            Collector.data.externalReferences,
            modname,
            [(children, loc, suffix), ...current]
          )
        | Some(`Local(stamp)) => {
          let current = maybeFound(Hashtbl.find(Collector.data.internalReferences), stamp) |? [];
          Hashtbl.replace(Collector.data.internalReferences, stamp, [loc, ...current])
        }
        };
      Collector.data.locations = [(loc, typ, definition), ...Collector.data.locations]
    };
    let addExplanation = (loc, text) => {
      Collector.data.explanations = [(loc, text), ...Collector.data.explanations]
    };
    let enter_signature_item = (item) =>
      switch item.sig_desc {
        | Tsig_attribute(({Asttypes.txt: "ocaml.explanation", loc}, PStr([{pstr_desc: Pstr_eval({pexp_desc: Pexp_constant(Const_string(doc, _))}, _)}]))) => {
          addExplanation(loc, doc)
        }
        | Tsig_attribute(({Asttypes.txt: "ocaml.doc" | "ocaml.text"}, PStr([{pstr_desc: Pstr_eval({pexp_desc: Pexp_constant(Const_string(doc, _))}, _)}]))) => {
          if (Collector.data.toplevelDocs == None) {
            Collector.data.toplevelDocs = Some(doc)
          } else {
            ()
          }
        }
      | Tsig_value({val_id: {stamp, name}, val_val: {val_type}, val_loc}) =>
        addStamp(stamp, name, val_loc, Value(val_type), None)
      | Tsig_type(decls) =>
        List.iter(
          ({typ_id: {stamp, name}, typ_loc, typ_type}) => {
            addStamp(stamp, name, typ_loc, Type(typ_type), None);
            switch (typ_type.type_kind) {
              | Types.Type_record(labels, _) => {
                labels |> List.iter(({Types.ld_id: {stamp, name: lname}, ld_type, ld_loc}) => {
                  addStamp(stamp, lname, ld_loc, Attribute(ld_type, name, typ_type), None)
                })
              }
              | Types.Type_variant(constructors) => {
                constructors |> List.iter(({Types.cd_id: {stamp, name: cname}, cd_loc} as cd) => {
                  addStamp(stamp, cname, cd_loc, Constructor(cd, name, typ_type), None)
                })

              }
              | _ => ()
            }
          },
          decls
        )
      /* TODO add support for these */
      /* | Tsig_include({incl_mod, incl_type}) => stampsFromTypesSignature(currentPath, incl_type) */
      /* | Tsig_module({md_id: {stamp, name}, md_type: {mty_desc: Tmty_signature(signature)}}) => {
           addStamp
           let (stamps) = stampsFromTypedtreeInterface(addToPath(currentPath, name), signature.sig_items);
           [(stamp, addToPath(currentPath, name) |> toFullPath(PModule)), ...stamps]
         } */
      | Tsig_module({md_id: {stamp, name}, md_loc, md_type}) =>
        addStamp(stamp, name, md_loc, Module([]), None)
      | _ => ()
      };
    let enter_structure_item = (item) =>
      Typedtree.(
        switch item.str_desc {
        | Tstr_attribute(({Asttypes.txt: "ocaml.explanation", loc}, PStr([{pstr_desc: Pstr_eval({pexp_desc: Pexp_constant(Const_string(doc, _))}, _)}]))) => {
          addExplanation(loc, doc)
        }
        | Tstr_attribute(({Asttypes.txt: "ocaml.doc" | "ocaml.text"}, PStr([{pstr_desc: Pstr_eval({pexp_desc: Pexp_constant(Const_string(doc, _))}, _)}]))) => {
          if (Collector.data.toplevelDocs == None) {
            Collector.data.toplevelDocs = Some(doc)
          } else {
            ()
          }
        }
        | Tstr_value(_rec, bindings) =>
          /* TODO limit toplevel value completions */
          bindings
          |> List.iter(
               (binding) =>
                 switch binding {
                 | {vb_attributes, vb_pat: {pat_type, pat_desc: Tpat_var({stamp, name}, {loc})}} =>
                   let docs = PrepareUtils.findDocAttribute(vb_attributes);
                   addStamp(stamp, name, loc, Value(pat_type), docs)
                 /* addLocation(loc, pat_type, None); */
                 | _ => ()
                 }
             )
        | Tstr_type(decls) =>
          decls
          |> List.iter(
               ({typ_attributes, typ_id: {stamp, name}, typ_type, typ_name: {loc}}) => {
                 let docs = PrepareUtils.findDocAttribute(typ_attributes);
                 addStamp(stamp, name, loc, Type(typ_type), docs);

                  switch (typ_type.type_kind) {
                    | Types.Type_record(labels, _) => {
                      labels |> List.iter(({Types.ld_id: {stamp: lstamp, name: lname}, ld_type, ld_loc}) => {
                        let shortLoc = Utils.clampLocation(ld_loc, String.length(lname));
                        addStamp(lstamp, lname, shortLoc, Attribute(ld_type, name, typ_type), docs);
                        if (maybeFound(Hashtbl.find(Collector.data.exported), name) == Some(stamp)) {
                          Collector.data.exportedSuffixes = [(lstamp, name, lname), ...Collector.data.exportedSuffixes];
                        };
                        addLocation(shortLoc, {Types.desc: Types.Tnil, level: 0, id: 0}, IsDefinition(lstamp));
                      })
                    }
                    | Types.Type_variant(constructors) => {
                      constructors |> List.iter(({Types.cd_id: {stamp: cstamp, name: cname}, cd_loc} as cd) => {
                        let shortLoc = Utils.clampLocation(cd_loc, String.length(cname));
                        addStamp(cstamp, cname, shortLoc, Constructor(cd, name, typ_type), docs);
                        addLocation(shortLoc, {Types.desc: Types.Tnil, level: 0, id: 0}, IsDefinition(cstamp));
                        if (maybeFound(Hashtbl.find(Collector.data.exported), name) == Some(stamp)) {
                          Collector.data.exportedSuffixes = [(cstamp, name, cname), ...Collector.data.exportedSuffixes];
                        };
                      })
                    }
                    | _ => ()
                  }
               }
             )
        | Tstr_module({
            mb_id: {stamp, name},
            mb_name: {loc},
            mb_expr: {
              mod_type,
              mod_desc:
                Tmod_structure(structure) |
                Tmod_constraint({mod_desc: Tmod_structure(structure)}, _, _, _)
            },
            mb_attributes
          }) =>
          let docs = PrepareUtils.findDocAttribute(mb_attributes);
          addOpenScope();
          addStamp(stamp, name, loc, Module(stampNames(structure.str_items)), docs)
        | Tstr_module({mb_attributes, mb_id: {stamp, name}, mb_name: {loc}, mb_expr: {mod_type}}) =>
          let docs = PrepareUtils.findDocAttribute(mb_attributes);
          addStamp(stamp, name, loc, ModuleWithDocs(Docs.forModuleType(x => x, mod_type)), docs)
        | Tstr_open({open_path, open_txt: {txt, loc}}) =>
          if (usesOpen(txt, open_path)) {
            addUse((open_path, TagModule), txt, loc)
          };
          addLocation(loc, {Types.desc: Types.Tnil, level: 0, id: 0}, Open(open_path));
          addOpen(open_path, loc)
        /* | Tstr_modtype */
        | _ => ()
        }
      );
    let leave_structure_item = (item) =>
      switch item.str_desc {
      | Tstr_module({
          mb_expr: {
            mod_desc: Tmod_structure(_) | Tmod_constraint({mod_desc: Tmod_structure(_)}, _, _, _)
          }
        }) =>
        popOpenScope()
      | _ => ()
      };
    let enter_core_type = (typ) =>
      /* open Typedtree; */
      /* Collector.add(~depth=depth^, typ.ctyp_type, typ.ctyp_loc); */
      switch typ.ctyp_desc {
      | Ttyp_constr(path, {txt, loc}, args) =>
        addLocation(loc, typ.ctyp_type, Path(path));
        if (usesOpen(txt, path)) {
          addUse((path, TagType), txt, loc)
        }
      /* Collector.ident((path, Type), loc) */
      | _ => ()
      };
    let enter_pattern = (pat) =>
      switch pat.pat_desc {
      | Tpat_alias(_, {stamp, name}, {txt, loc})
      | Tpat_var({stamp, name}, {txt, loc}) =>
        addStamp(stamp, name, loc, Value(pat.pat_type), None);
        addLocation(loc, pat.pat_type, IsDefinition(stamp))
      | Tpat_construct({txt, loc}, {cstr_name, cstr_loc, cstr_res}, args) =>
        switch (dig(cstr_res).Types.desc) {
        | Tconstr(path, args, _) =>
          let (constructorName, typeTxt) = handleConstructor(path, txt);
          if (usesOpen(typeTxt, path)) {
            addUse((path, TagConstructor(constructorName)), typeTxt, loc)
          };
          addLocation(loc, pat.pat_type, ConstructorDefn(path, cstr_name, cstr_loc))
        | _ => ()
        }
      | Tpat_record(items, isClosed) =>
        items
        |> List.iter(
             (({Asttypes.txt, loc}, {Types.lbl_res, lbl_name, lbl_loc}, value)) =>
               switch (dig(lbl_res).Types.desc) {
               | Tconstr(path, args, _) =>
                 addLocation(loc, lbl_res, AttributeDefn(path, lbl_name, lbl_loc));
                 let typeTxt = handleRecord(path, txt);
                 if (usesOpen(typeTxt, path)) {
                   addUse((path, TagAttribute(lbl_name)), typeTxt, loc)
                 }
               | _ => ()
               }
           )
      | _ => ()
      };
    let enter_expression = (expr) => {
      expr.exp_attributes |> List.iter(attr => switch attr {
        | ({Asttypes.txt: "ocaml.explanation", loc}, Parsetree.PStr([{pstr_desc: Pstr_eval({pexp_desc: Pexp_constant(Const_string(doc, _))}, _)}])) => {
          addExplanation(loc, doc)
        }
        | _ => ()
      });

      switch expr.exp_desc {
      | Texp_for({stamp, name}, {ppat_loc}, {exp_type}, _, _, contents) =>
        addLocation(ppat_loc, exp_type, IsDefinition(stamp));
        addScope(rangeOfLoc(contents.exp_loc));
        addStamp(stamp, name, ppat_loc, Value(exp_type), None);
        popScope()
      /* JSX fix */
      | Texp_ident(
          path,
          {txt, loc},
          _
        )
          when locationSize(loc) != String.length(Longident.flatten(txt) |> String.concat(".")) =>
        ()
      | Texp_ident(path, {txt, loc}, _) =>
        addLocation(loc, expr.exp_type, Path(path));
        if (usesOpen(txt, path)) {
          addUse((path, TagValue), txt, loc)
        }
      | Texp_field(inner, {txt, loc}, {lbl_name, lbl_res, lbl_loc}) =>
        switch (dig(lbl_res).Types.desc) {
        | Tconstr(path, args, _) =>
          addLocation(loc, expr.exp_type, AttributeDefn(path, lbl_name, lbl_loc));
          let typeTxt = handleRecord(path, txt);
          if (usesOpen(typeTxt, path)) {
            addUse((path, TagAttribute(lbl_name)), typeTxt, loc)
          }
        | _ => ()
        }
      /* JSX string */
      | Texp_constant(Const_string(text, None)) when locationSize(expr.exp_loc) != String.length(text) => ()
      | Texp_constant(_) => addLocation(expr.exp_loc, expr.exp_type, IsConstant)
      | Texp_record(items, ext) =>
        items
        |> List.iter(
             (({Asttypes.txt, loc}, {Types.lbl_loc, lbl_name, lbl_res}, ex)) =>
               switch (dig(lbl_res).Types.desc) {
               | Tconstr(path, args, _) =>
                 addLocation(loc, ex.exp_type, AttributeDefn(path, lbl_name, lbl_loc));
                 let typeTxt = handleRecord(path, txt);
                 if (usesOpen(typeTxt, path)) {
                   addUse((path, TagAttribute(lbl_name)), typeTxt, loc)
                 }
               | _ => ()
               }
           )
      /* Skip list literals */
      | Texp_construct(
          {txt: Lident("()"), loc: {loc_start: {pos_cnum: cstart}, loc_end: {pos_cnum: cend}}},
          {cstr_name, cstr_loc, cstr_res},
          args
        )
          when cend - cstart != 2 =>
        ()
      | Texp_construct(
          {txt: Lident("::"), loc: {loc_start: {pos_cnum: cstart}, loc_end: {pos_cnum: cend}}},
          {cstr_name, cstr_loc, cstr_res},
          args
        )
          when cend - cstart != 2 =>
        ()
      | Texp_construct({txt, loc}, {cstr_name, cstr_loc, cstr_res}, args) =>
        switch (dig(cstr_res).Types.desc) {
        | Tconstr(path, args, _) =>
          addLocation(loc, expr.exp_type, ConstructorDefn(path, cstr_name, cstr_loc));
          let (constructorName, typeTxt) = handleConstructor(path, txt);
          if (usesOpen(typeTxt, path)) {
            addUse((path, TagConstructor(constructorName)), typeTxt, loc)
          }
        | _ => ()
        }
      | Texp_let(recFlag, bindings, expr) =>
        let start =
          Asttypes.Recursive == recFlag ?
            List.hd(bindings).vb_loc.loc_start : expr.exp_loc.loc_start;
        addScope((posOfLexing(start), posOfLexing(expr.exp_loc.loc_end)))
      | Texp_function(label, cases, _) => addScope(rangeOfLoc(expr.exp_loc))
      | _ => ()
      };
    };
    let leave_expression = (expr) =>
      switch expr.exp_desc {
      | Texp_let(recFlag, bindings, expr) => popScope()
      | Texp_function(_) => popScope()
      | _ => ()
      };
  };

  let process = (cmt) => {
    let data = {
      toplevelDocs: None,
      stamps: Hashtbl.create(100),
      internalReferences: Hashtbl.create(100),
      externalReferences: Hashtbl.create(100),
      exportedSuffixes: [],
      exported: Hashtbl.create(10),
      allOpens: [],
      topLevel: [],
      locations: [],
      explanations: [],
    };
    let allOpens = ref([]);
    module IterIter =
      TypedtreeIter.MakeIterator(
        (
          F(
            {
              let data = data;
              let allOpens = allOpens;
            }
          )
        )
      );
    let structure = (items) => {
      let names = stampNames(items);
      names |> List.iter(((name, stamp)) => Hashtbl.replace(data.exported, name, stamp));
      data.topLevel = names;
      List.iter(IterIter.iter_structure_item, items)
    };
    let iter_part = (part) =>
      switch part {
      | Cmt_format.Partial_structure(str) =>
        IterIter.iter_structure(str);
        stampNames(str.str_items)
      | Partial_structure_item(str) =>
        IterIter.iter_structure_item(str);
        stampNames([str])
      | Partial_signature(str) =>
        IterIter.iter_signature(str);
        []
      | Partial_signature_item(str) =>
        IterIter.iter_signature_item(str);
        []
      | Partial_expression(expression) =>
        IterIter.iter_expression(expression);
        []
      | Partial_pattern(pattern) =>
        IterIter.iter_pattern(pattern);
        []
      | Partial_class_expr(class_expr) =>
        IterIter.iter_class_expr(class_expr);
        []
      | Partial_module_type(module_type) =>
        IterIter.iter_module_type(module_type);
        []
      };
    switch cmt {
    | Cmt_format.Implementation(str) => structure(str.str_items)
    | Cmt_format.Interface(sign) => IterIter.iter_signature(sign)
    | Cmt_format.Partial_implementation(parts)
    | Cmt_format.Partial_interface(parts) =>
      let names = Array.map(iter_part, parts) |> Array.to_list |> List.concat;
      names |> List.iter(((name, stamp)) => Hashtbl.replace(data.exported, name, stamp));
      data.topLevel = names
    | _ => failwith("Not a valid cmt file")
    };
    data.locations = List.rev(data.locations);
    data.explanations = List.rev(data.explanations);
    /* allOpens^ |> List.iter(({used, path, loc}) => {
         Log.log("An Open! " ++ string_of_int(List.length(used)));
       }); */
    data.allOpens = allOpens^;
    data
  };
};

let process = Get.process;