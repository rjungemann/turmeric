-- fib_recursive -- naive doubly-recursive fibonacci on Int.
import System.Environment (getArgs)
fib :: Int -> Int
fib n | n <= 1 = n
      | otherwise = fib (n - 1) + fib (n - 2)
main :: IO ()
main = do
  args <- getArgs
  let n = case args of (a:_) -> read a; _ -> 25 :: Int
  print (fib n)
