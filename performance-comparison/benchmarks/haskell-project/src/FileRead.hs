-- file_read -- write an N-byte 0xCD file, read back in 4 KiB chunks.
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
import System.IO
import System.Directory (removeFile)
import qualified Data.ByteString as BS
main :: IO ()
main = do
  args <- getArgs
  let n = case args of (a:_) -> read a; _ -> 1048576 :: Int
      path = "/tmp/bench_io_read_hs.bin"
      chunk = BS.replicate 4096 0xCD
  h <- openBinaryFile path WriteMode
  let put !rem_
        | rem_ <= 0 = return ()
        | otherwise = BS.hPut h (BS.take (min 4096 rem_) chunk) >> put (rem_ - 4096)
  put n
  hClose h
  r <- openBinaryFile path ReadMode
  let get !total = do
        b <- BS.hGet r 4096
        if BS.null b then return total else get (total + BS.length b)
  total <- get 0
  hClose r
  removeFile path
  print total
