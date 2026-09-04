-- file_write -- N bytes in 4 KiB chunks, remove, print byte count.
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
import System.IO
import System.Directory (removeFile)
import qualified Data.ByteString as BS
main :: IO ()
main = do
  args <- getArgs
  let n = case args of (a:_) -> read a; _ -> 1048576 :: Int
      path = "/tmp/bench_io_write_hs.bin"
      buf = BS.replicate 4096 0xAB
  h <- openBinaryFile path WriteMode
  let go !written
        | written >= n = return ()
        | otherwise = do
            let chunk = min 4096 (n - written)
            BS.hPut h (BS.take chunk buf)
            go (written + chunk)
  go 0
  hClose h
  removeFile path
  print n
