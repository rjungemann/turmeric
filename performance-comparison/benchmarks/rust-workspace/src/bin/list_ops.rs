// list_ops -- prepend N ints onto a real linked cons chain (Box), sum by
// walking it, then drop iteratively so a deep list doesn't blow the stack
// on a recursive Drop impl (docs/methodology.md: linked structure, never Vec).
use std::env;

struct Cons {
    head: i64,
    tail: Link,
}

type Link = Option<Box<Cons>>;

fn build_list(n: i64) -> Link {
    let mut acc: Link = None;
    let mut i: i64 = 0;
    while i < n {
        acc = Some(Box::new(Cons { head: i, tail: acc }));
        i += 1;
    }
    acc
}

fn sum_list(mut lst: Link) -> i64 {
    let mut acc: i64 = 0;
    while let Some(cell) = lst {
        acc += cell.head;
        lst = cell.tail;
    }
    acc
}

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(1000);
    let lst = build_list(n);
    println!("{}", sum_list(lst));
}
